#!/usr/bin/env node

import { mkdtempSync, readFileSync, rmSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';

const DISPATCH_MARKER = '<!-- UAI-SPARK-DISPATCH v1';
const RESULT_MARKER = '<!-- UAI-SPARK-RESULT v1';
const MAX_COMMENT_CHARS = 60000;

function usage() {
  return `UCI Arena local Codex/Spark relay

Usage:
  node tools/agent-relay.mjs --repo OWNER/REPO --pr NUMBER --checkout PATH [options]

Required:
  --repo OWNER/REPO       GitHub repository, e.g. iteathen/uci-arena-installer
  --pr NUMBER             Pull request carrying dispatch/result comments
  --checkout PATH         Existing local checkout used only for git metadata/fetch/worktree creation

Options:
  --profile NAME          Codex profile to use (default: spark)
  --author LOGIN          Only accept dispatches authored by this GitHub login (default: repo owner)
  --worktree-root PATH    Parent directory for relay-created detached worktrees
  --poll-seconds N        Watch interval (default: 15)
  --once                  Process at most one pending dispatch, then exit
  --add-dir PATH          Additional Codex writable directory; repeatable
  --self-test             Run parser/validation self-tests only
  --help                  Show this help

Dispatch comment format:
  <!-- UAI-SPARK-DISPATCH v1
  {"dispatch_id":"...","repo":"OWNER/REPO","pr":140,
   "expected_head":"<sha>","target_branch":"branch/name",
   "allowed_paths":["path/a","path/b"],"push_statuses":["completed"]}
  -->
  <full Spark prompt>

The relay creates/reuses a clean detached worktree at expected_head, invokes
Codex non-interactively with the selected profile and workspace-write sandbox,
then posts a durable UAI-SPARK-RESULT comment. It never merges or marks a PR ready.
If Codex commits a result whose STATUS is allowed by push_statuses, the relay
pushes only after exact-head, ancestry, clean-tree, allowed-path, and remote-head guards pass.
`;
}

function parseArgs(argv) {
  const out = { once: false, selfTest: false, profile: 'spark', addDirs: [], pollSeconds: 15 };
  const valueKeys = new Set(['repo', 'pr', 'checkout', 'profile', 'author', 'worktree-root', 'poll-seconds', 'add-dir']);
  for (let i = 0; i < argv.length; i += 1) {
    const token = argv[i];
    if (token === '--once') out.once = true;
    else if (token === '--self-test') out.selfTest = true;
    else if (token === '--help' || token === '-h') out.help = true;
    else if (token.startsWith('--')) {
      const key = token.slice(2);
      if (!valueKeys.has(key)) throw new Error(`unknown option ${token}`);
      const value = argv[++i];
      if (value === undefined) throw new Error(`missing value for ${token}`);
      if (key === 'add-dir') out.addDirs.push(value);
      else if (key === 'worktree-root') out.worktreeRoot = value;
      else if (key === 'poll-seconds') out.pollSeconds = Number(value);
      else out[key] = value;
    } else {
      throw new Error(`unexpected argument ${token}`);
    }
  }
  if (out.pr !== undefined) out.pr = Number(out.pr);
  return out;
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    encoding: 'utf8',
    windowsHide: true,
    maxBuffer: 16 * 1024 * 1024,
    ...options,
  });
  if (result.error) throw result.error;
  return result;
}

function mustRun(command, args, options = {}) {
  const result = run(command, args, options);
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} failed (${result.status}): ${(result.stderr || result.stdout || '').trim()}`);
  }
  return result.stdout.trim();
}

function ghJson(args, input) {
  const result = mustRun('gh', args, input === undefined ? {} : { input });
  return result ? JSON.parse(result) : null;
}

function fetchComments(repo, pr) {
  const comments = [];
  for (let page = 1; ; page += 1) {
    const rows = ghJson(['api', `repos/${repo}/issues/${pr}/comments?per_page=100&page=${page}`]);
    if (!Array.isArray(rows)) throw new Error('GitHub comments response was not an array');
    comments.push(...rows);
    if (rows.length < 100) break;
  }
  return comments;
}

function parseMarkedComment(body, marker) {
  if (typeof body !== 'string') return null;
  const start = body.indexOf(marker);
  if (start < 0) return null;
  const headerStart = start + marker.length;
  const end = body.indexOf('-->', headerStart);
  if (end < 0) return null;
  const rawHeader = body.slice(headerStart, end).trim();
  let metadata;
  try {
    metadata = JSON.parse(rawHeader);
  } catch {
    return null;
  }
  return { metadata, text: body.slice(end + 3).trim() };
}

function normalizedAllowedPath(value) {
  if (typeof value !== 'string' || value.length === 0) throw new Error('allowed_paths must contain nonempty strings');
  const normalized = value.replaceAll('\\', '/').replace(/^\.\//, '');
  if (normalized.startsWith('/') || normalized.includes('../') || normalized === '..') {
    throw new Error(`invalid allowed path ${value}`);
  }
  return normalized;
}

function validateDispatch(dispatch, repo, pr) {
  const m = dispatch.metadata;
  if (!m || m.version !== 1) throw new Error('dispatch metadata version must be 1');
  if (typeof m.dispatch_id !== 'string' || !/^[A-Za-z0-9._-]{1,120}$/.test(m.dispatch_id)) throw new Error('invalid dispatch_id');
  if (m.repo !== repo) throw new Error(`dispatch repo mismatch: ${m.repo}`);
  if (Number(m.pr) !== pr) throw new Error(`dispatch PR mismatch: ${m.pr}`);
  if (typeof m.expected_head !== 'string' || !/^[0-9a-f]{40}$/.test(m.expected_head)) throw new Error('invalid expected_head');
  if (typeof m.target_branch !== 'string' || m.target_branch.length === 0 || /[\s~^:?*[\\]/.test(m.target_branch)) throw new Error('invalid target_branch');
  if (!Array.isArray(m.allowed_paths) || m.allowed_paths.length === 0) throw new Error('allowed_paths is required');
  m.allowed_paths = [...new Set(m.allowed_paths.map(normalizedAllowedPath))];
  const statuses = m.push_statuses ?? ['completed'];
  if (!Array.isArray(statuses) || statuses.some((s) => !['completed', 'failed'].includes(s))) throw new Error('push_statuses may contain only completed and failed');
  m.push_statuses = [...new Set(statuses)];
  if (!dispatch.text) throw new Error('dispatch prompt is empty');
  return dispatch;
}

function resultDispatchIds(comments, author) {
  const ids = new Set();
  for (const comment of comments) {
    if (comment?.user?.login !== author) continue;
    const parsed = parseMarkedComment(comment.body, RESULT_MARKER);
    if (parsed?.metadata?.dispatch_id) ids.add(parsed.metadata.dispatch_id);
  }
  return ids;
}

function pendingDispatches(comments, author, repo, pr) {
  const done = resultDispatchIds(comments, author);
  const pending = [];
  for (const comment of comments) {
    if (comment?.user?.login !== author) continue;
    const parsed = parseMarkedComment(comment.body, DISPATCH_MARKER);
    if (!parsed) continue;
    try {
      validateDispatch(parsed, repo, pr);
    } catch (error) {
      console.error(`[relay] ignored invalid dispatch comment ${comment.id}: ${error.message}`);
      continue;
    }
    if (!done.has(parsed.metadata.dispatch_id)) pending.push({ ...parsed, commentId: comment.id, createdAt: comment.created_at });
  }
  pending.sort((a, b) => a.commentId - b.commentId);
  return pending;
}

function currentPr(repo, pr) {
  return ghJson(['api', `repos/${repo}/pulls/${pr}`]);
}

function verifyPrForDispatch(prInfo, m) {
  if (prInfo.state !== 'open') throw new Error(`PR is ${prInfo.state}, expected open`);
  if (prInfo.head?.sha !== m.expected_head) throw new Error(`remote PR head moved: expected ${m.expected_head}, observed ${prInfo.head?.sha}`);
  if (prInfo.head?.ref !== m.target_branch) throw new Error(`PR head branch mismatch: expected ${m.target_branch}, observed ${prInfo.head?.ref}`);
}

function sanitizeId(id) {
  return id.replace(/[^A-Za-z0-9._-]/g, '-').slice(0, 100);
}

function worktreePath(root, dispatchId) {
  return path.join(root, sanitizeId(dispatchId));
}

function prepareWorktree(checkout, root, m) {
  mustRun('git', ['-C', checkout, 'fetch', 'origin', m.target_branch]);
  const wt = worktreePath(root, m.dispatch_id);
  mkdirSync(root, { recursive: true });
  if (!existsSync(wt)) {
    mustRun('git', ['-C', checkout, 'worktree', 'add', '--detach', wt, m.expected_head]);
  }
  const head = mustRun('git', ['-C', wt, 'rev-parse', 'HEAD']);
  const status = mustRun('git', ['-C', wt, 'status', '--porcelain']);
  if (head !== m.expected_head) throw new Error(`existing relay worktree ${wt} is at ${head}, expected ${m.expected_head}`);
  if (status !== '') throw new Error(`existing relay worktree ${wt} is dirty; refusing to alter it`);
  return wt;
}

function statusFromReport(report) {
  const match = String(report).match(/^STATUS:\s*(completed|failed|blocked_architecture|blocked_dependency)\b/im);
  return match ? match[1].toLowerCase() : null;
}

function changedFiles(wt, base, head) {
  if (base === head) return [];
  const output = mustRun('git', ['-C', wt, 'diff', '--name-only', `${base}..${head}`]);
  return output ? output.split(/\r?\n/).filter(Boolean).map((p) => p.replaceAll('\\', '/')) : [];
}

function guardAndMaybePush({ repo, pr, wt, metadata, report, codexStatus }) {
  const outcome = {
    pushed: false,
    push_status: 'not_attempted',
    final_head: mustRun('git', ['-C', wt, 'rev-parse', 'HEAD']),
    changed_files: [],
  };
  const reportStatus = statusFromReport(report);
  outcome.report_status = reportStatus;
  if (codexStatus !== 0) {
    outcome.push_status = 'codex_exit_nonzero';
    return outcome;
  }
  const treeStatus = mustRun('git', ['-C', wt, 'status', '--porcelain']);
  if (treeStatus !== '') {
    outcome.push_status = 'worktree_dirty';
    return outcome;
  }
  const ancestor = run('git', ['-C', wt, 'merge-base', '--is-ancestor', metadata.expected_head, outcome.final_head]);
  if (ancestor.status !== 0) {
    outcome.push_status = 'initial_head_not_ancestor';
    return outcome;
  }
  outcome.changed_files = changedFiles(wt, metadata.expected_head, outcome.final_head);
  const allowed = new Set(metadata.allowed_paths);
  const outside = outcome.changed_files.filter((file) => !allowed.has(file));
  if (outside.length) {
    outcome.push_status = `scope_violation:${outside.join(',')}`;
    return outcome;
  }
  if (outcome.final_head === metadata.expected_head) {
    outcome.push_status = 'no_new_commit';
    return outcome;
  }
  if (!metadata.push_statuses.includes(reportStatus)) {
    outcome.push_status = `report_status_not_pushable:${reportStatus ?? 'missing'}`;
    return outcome;
  }
  const beforePush = currentPr(repo, pr);
  verifyPrForDispatch(beforePush, metadata);
  const push = run('git', ['-C', wt, 'push', 'origin', `HEAD:refs/heads/${metadata.target_branch}`]);
  if (push.status !== 0) {
    outcome.push_status = `push_failed:${(push.stderr || push.stdout || '').trim()}`;
    return outcome;
  }
  const afterPush = currentPr(repo, pr);
  if (afterPush.head?.sha !== outcome.final_head) {
    outcome.push_status = `remote_verification_failed:${afterPush.head?.sha}`;
    return outcome;
  }
  outcome.pushed = true;
  outcome.push_status = 'pushed_and_verified';
  return outcome;
}

function postComment(repo, pr, body) {
  const payload = JSON.stringify({ body });
  ghJson(['api', '--method', 'POST', `repos/${repo}/issues/${pr}/comments`, '--input', '-'], payload);
}

function bounded(value, max = MAX_COMMENT_CHARS) {
  const text = String(value ?? '');
  if (text.length <= max) return text;
  return `${text.slice(0, max - 120)}\n\n[relay truncated ${text.length - max + 120} characters]`;
}

function resultComment(m, wt, codexStatus, report, stderr, push) {
  const metadata = {
    dispatch_id: m.dispatch_id,
    initial_head: m.expected_head,
    final_head: push.final_head,
    report_status: push.report_status,
    push_status: push.push_status,
  };
  const diagnostics = stderr?.trim() ? `\n\n<details><summary>Codex stderr</summary>\n\n\`\`\`text\n${bounded(stderr, 6000)}\n\`\`\`\n</details>` : '';
  return `${RESULT_MARKER}\n${JSON.stringify(metadata)}\n-->\n### Local Spark relay result\n\n- Dispatch: \`${m.dispatch_id}\`\n- Worktree: \`${wt}\`\n- Codex exit: \`${codexStatus}\`\n- Push guard: \`${push.push_status}\`\n- Final HEAD: \`${push.final_head}\`\n- Changed files: ${push.changed_files.length ? push.changed_files.map((x) => `\`${x}\``).join(', ') : '_none_'}\n\n<details open><summary>Codex final report</summary>\n\n\`\`\`text\n${bounded(report, 45000)}\n\`\`\`\n</details>${diagnostics}`;
}

function blockedComment(m, reason) {
  return `${RESULT_MARKER}\n${JSON.stringify({ dispatch_id: m.dispatch_id, report_status: 'relay_blocked', push_status: 'not_attempted' })}\n-->\n### Local Spark relay blocked\n\n\`${bounded(reason, 12000)}\``;
}

function createPythonGuard(temp) {
  const guard = path.join(temp, 'no-python');
  mkdirSync(guard, { recursive: true });
  const names = ['python', 'python3', 'py', 'pip', 'pip3'];
  const cmd = '@echo off\r\necho ERROR: Python is forbidden by the UCI Arena Spark relay. 1>&2\r\nexit /b 126\r\n';
  for (const name of names) {
    writeFileSync(path.join(guard, `${name}.cmd`), cmd);
    writeFileSync(path.join(guard, `${name}.exe`), '');
  }
  const inherited = process.env.PATHEXT ?? '.COM;.EXE;.BAT;.CMD';
  const extensions = inherited.split(';').filter(Boolean).map((x) => x.toUpperCase());
  const ordered = ['.CMD', '.BAT', '.COM', '.EXE', ...extensions]
    .filter((x, i, a) => a.indexOf(x) === i)
    .join(';');
  return {
    guard,
    env: {
      ...process.env,
      PATH: `${guard}${path.delimiter}${process.env.PATH ?? ''}`,
      PATHEXT: ordered,
    },
  };
}
function guardedPrompt(dispatch, wt) {
  return `LOCAL RELAY BOUNDARY — authoritative for this run\n\nYou are the bounded Spark implementation/validation agent. The relay has already created a clean detached worktree at the dispatch expected HEAD:\n${wt}\n\nOperate ONLY in the current worktree. Do not create, reset, clean, stash, delete, or repurpose other worktrees.\nNEVER USE PYTHON: do not run python, python3, py, pip, pip3, .py helpers, Python diagnostics, file manipulation, binary inspection, or tests.\nDo not push, merge, mark a PR ready, close issues, select the next issue, or change architecture unless the dispatch explicitly authorizes an architectural decision.\nIf you make a successful bounded correction, commit it locally. The relay owns guarded pushing after your final report.\nDo not use network operations unless the dispatch explicitly requires them; the relay owns GitHub coordination.\nFollow AGENTS.md and repository authority exactly.\n\n--- BEGIN SOL DISPATCH ---\n${dispatch.text}\n--- END SOL DISPATCH ---\n`;
}

function executeDispatch(config, dispatch) {
  const m = dispatch.metadata;
  try {
    verifyPrForDispatch(currentPr(config.repo, config.pr), m);
    const wt = prepareWorktree(config.checkout, config.worktreeRoot, m);
    const temp = mkdtempSync(path.join(tmpdir(), 'uai-spark-relay-'));
    const outputFile = path.join(temp, 'last-message.txt');
    const guarded = createPythonGuard(temp);
    const args = [];
    for (const dir of config.addDirs) args.push('--add-dir', dir);
    args.push('exec', '-C', wt, '--profile', config.profile, '--sandbox', 'workspace-write', '--ephemeral', '--output-last-message', outputFile, '-');
    const codex = run('codex', args, { input: guardedPrompt(dispatch, wt), env: guarded.env });
    const report = existsSync(outputFile) ? readFileSync(outputFile, 'utf8') : (codex.stdout || '');
    const push = guardAndMaybePush({ repo: config.repo, pr: config.pr, wt, metadata: m, report, codexStatus: codex.status ?? 1 });
    postComment(config.repo, config.pr, resultComment(m, wt, codex.status ?? 1, report, codex.stderr, push));
    rmSync(temp, { recursive: true, force: true });
    console.log(`[relay] completed dispatch ${m.dispatch_id}: ${push.push_status}`);
    return true;
  } catch (error) {
    console.error(`[relay] dispatch ${m.dispatch_id} blocked: ${error.message}`);
    try { postComment(config.repo, config.pr, blockedComment(m, error.message)); } catch (postError) { console.error(`[relay] failed to post blocked result: ${postError.message}`); }
    return false;
  }
}

function validateConfig(config) {
  if (!config.repo || !/^[^/\s]+\/[^/\s]+$/.test(config.repo)) throw new Error('--repo OWNER/REPO is required');
  if (!Number.isInteger(config.pr) || config.pr <= 0) throw new Error('--pr positive integer is required');
  if (!config.checkout) throw new Error('--checkout PATH is required');
  if (!Number.isFinite(config.pollSeconds) || config.pollSeconds < 5) throw new Error('--poll-seconds must be at least 5');
  const owner = config.repo.split('/')[0];
  if (!config.author) config.author = owner;
  if (!config.worktreeRoot) config.worktreeRoot = path.resolve(config.checkout, '..', 'uai-agent-relay-worktrees');
}

function preflightLocal(config) {
  mustRun('gh', ['auth', 'status']);
  mustRun('codex', ['--version']);
  mustRun('git', ['-C', config.checkout, 'rev-parse', '--show-toplevel']);
}

function sleep(ms) {
  const until = Date.now() + ms;
  while (Date.now() < until) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, Math.min(1000, until - Date.now()));
}

function selfTest() {
  const dispatchBody = `${DISPATCH_MARKER}\n{"version":1,"dispatch_id":"t-1","repo":"o/r","pr":7,"expected_head":"${'a'.repeat(40)}","target_branch":"b","allowed_paths":["x/y"],"push_statuses":["completed"]}\n-->\nDo the bounded task.`;
  const parsed = parseMarkedComment(dispatchBody, DISPATCH_MARKER);
  validateDispatch(parsed, 'o/r', 7);
  if (parsed.text !== 'Do the bounded task.') throw new Error('prompt parse failed');
  const comments = [
    { id: 1, user: { login: 'o' }, body: dispatchBody, created_at: 'x' },
    { id: 2, user: { login: 'o' }, body: `${RESULT_MARKER}\n{"dispatch_id":"other"}\n-->\nresult`, created_at: 'y' },
  ];
  const pending = pendingDispatches(comments, 'o', 'o/r', 7);
  if (pending.length !== 1 || pending[0].metadata.dispatch_id !== 't-1') throw new Error('pending dispatch selection failed');
  if (statusFromReport('STATUS: completed\nHEAD SHA: abc') !== 'completed') throw new Error('status parse failed');
  if (normalizedAllowedPath('.\\a\\b') !== 'a/b') throw new Error('path normalization failed');
  let rejected = false;
  try { normalizedAllowedPath('../bad'); } catch { rejected = true; }
  if (!rejected) throw new Error('path traversal rejection failed');
  console.log('agent-relay self-test: PASS');
}

function main() {
  const config = parseArgs(process.argv.slice(2));
  if (config.help) { process.stdout.write(usage()); return; }
  if (config.selfTest) { selfTest(); return; }
  validateConfig(config);
  preflightLocal(config);
  console.log(`[relay] watching ${config.repo} PR #${config.pr} as ${config.author}; profile=${config.profile}`);
  for (;;) {
    const comments = fetchComments(config.repo, config.pr);
    const pending = pendingDispatches(comments, config.author, config.repo, config.pr);
    if (pending.length) {
      executeDispatch(config, pending[0]);
      if (config.once) return;
    } else if (config.once) {
      console.log('[relay] no pending dispatch');
      return;
    }
    sleep(config.pollSeconds * 1000);
  }
}

try {
  main();
} catch (error) {
  console.error(`[relay] fatal: ${error.message}`);
  process.exitCode = 1;
}
