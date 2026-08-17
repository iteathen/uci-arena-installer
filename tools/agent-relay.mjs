#!/usr/bin/env node
import {existsSync,mkdirSync,mkdtempSync,readFileSync,rmSync,writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import process from 'node:process';
import {spawnSync} from 'node:child_process';

const DISPATCH='<!-- UAI-SPARK-DISPATCH v1';
const RESULT='<!-- UAI-SPARK-RESULT v1';

function args(argv){
  const c={profile:'spark',poll:15,once:false,addDirs:[]};
  const value=new Set(['repo','pr','checkout','profile','author','worktree-root','poll-seconds','add-dir']);
  for(let i=0;i<argv.length;i++){
    const t=argv[i];
    if(t==='--once') c.once=true;
    else if(t==='--self-test') c.selfTest=true;
    else if(t==='--help'||t==='-h') c.help=true;
    else if(t.startsWith('--')){
      const k=t.slice(2); if(!value.has(k)) throw new Error(`unknown option ${t}`);
      const v=argv[++i]; if(v===undefined) throw new Error(`missing value for ${t}`);
      if(k==='pr') c.pr=Number(v); else if(k==='poll-seconds') c.poll=Number(v);
      else if(k==='worktree-root') c.worktreeRoot=v; else if(k==='add-dir') c.addDirs.push(v); else c[k]=v;
    } else throw new Error(`unexpected argument ${t}`);
  }
  return c;
}
function run(cmd,a,o={}){const r=spawnSync(cmd,a,{encoding:'utf8',windowsHide:true,maxBuffer:16*1024*1024,...o});if(r.error)throw r.error;return r;}
function must(cmd,a,o={}){const r=run(cmd,a,o);if(r.status!==0)throw new Error(`${cmd} ${a.join(' ')} failed (${r.status}): ${(r.stderr||r.stdout||'').trim()}`);return r.stdout.trim();}
function gh(a,input){const out=must('gh',a,input===undefined?{}:{input});return out?JSON.parse(out):null;}
function help(){return `UCI Arena local Spark relay\n\nnode tools/agent-relay.mjs --repo OWNER/REPO --pr N --checkout PATH [--profile spark] [--once]\n\nReads owner-authored UAI-SPARK-DISPATCH v1 PR comments, exact-head guards them, creates a clean detached worktree, runs Codex in workspace-write, posts UAI-SPARK-RESULT v1, and—only for an allowed successful edit—performs the commit and guarded push itself. It never merges or marks a PR ready.\n`;}
function commentPages(repo,pr){const all=[];for(let p=1;;p++){const x=gh(['api',`repos/${repo}/issues/${pr}/comments?per_page=100&page=${p}`]);if(!Array.isArray(x))throw new Error('comments response is not an array');all.push(...x);if(x.length<100)return all;}}
function marked(body,marker){if(typeof body!=='string')return null;const s=body.indexOf(marker);if(s<0)return null;const e=body.indexOf('-->',s+marker.length);if(e<0)return null;try{return{meta:JSON.parse(body.slice(s+marker.length,e).trim()),text:body.slice(e+3).trim()};}catch{return null;}}
function norm(p){if(typeof p!=='string'||!p)throw new Error('empty allowed path');const n=p.replaceAll('\\','/').replace(/^\.\//,'');if(n.startsWith('/')||n==='..'||n.includes('../'))throw new Error(`invalid allowed path ${p}`);return n;}
function validate(d,repo,pr){const m=d.meta;if(!m||m.version!==1)throw new Error('dispatch version must be 1');if(!/^[A-Za-z0-9._-]{1,120}$/.test(m.dispatch_id||''))throw new Error('invalid dispatch_id');if(m.repo!==repo||Number(m.pr)!==pr)throw new Error('dispatch target mismatch');if(!/^[0-9a-f]{40}$/.test(m.expected_head||''))throw new Error('invalid expected_head');if(typeof m.target_branch!=='string'||/[\s~^:?*[\\]/.test(m.target_branch))throw new Error('invalid target_branch');if(!Array.isArray(m.allowed_paths)||!m.allowed_paths.length)throw new Error('allowed_paths required');m.allowed_paths=[...new Set(m.allowed_paths.map(norm))];m.push_statuses=m.push_statuses??['completed'];if(m.push_statuses.some(x=>!['completed','failed'].includes(x)))throw new Error('invalid push_statuses');if(!d.text)throw new Error('empty dispatch prompt');return d;}
function pending(comments,author,repo,pr){const done=new Set();for(const c of comments){if(c?.user?.login!==author)continue;const r=marked(c.body,RESULT);if(r?.meta?.dispatch_id)done.add(r.meta.dispatch_id);}const out=[];for(const c of comments){if(c?.user?.login!==author)continue;const d=marked(c.body,DISPATCH);if(!d)continue;try{validate(d,repo,pr);}catch(e){console.error(`[relay] ignore comment ${c.id}: ${e.message}`);continue;}if(!done.has(d.meta.dispatch_id))out.push({...d,id:c.id});}return out.sort((a,b)=>a.id-b.id);}
function pr(repo,n){return gh(['api',`repos/${repo}/pulls/${n}`]);}
function verify(p,m){if(p.state!=='open')throw new Error(`PR is ${p.state}`);if(p.head?.sha!==m.expected_head)throw new Error(`PR head moved: expected ${m.expected_head}, saw ${p.head?.sha}`);if(p.head?.ref!==m.target_branch)throw new Error(`PR branch changed: expected ${m.target_branch}, saw ${p.head?.ref}`);}
function wtPath(root,id){return path.join(root,id.replace(/[^A-Za-z0-9._-]/g,'-').slice(0,100));}
function prepare(checkout,root,m){must('git',['-C',checkout,'fetch','origin',m.target_branch]);mkdirSync(root,{recursive:true});const w=wtPath(root,m.dispatch_id);if(!existsSync(w))must('git',['-C',checkout,'worktree','add','--detach',w,m.expected_head]);if(must('git',['-C',w,'rev-parse','HEAD'])!==m.expected_head)throw new Error(`relay worktree ${w} has wrong HEAD`);if(must('git',['-C',w,'status','--porcelain']))throw new Error(`relay worktree ${w} is dirty`);return w;}
function status(report){return String(report).match(/^STATUS:\s*(completed|failed|blocked_architecture|blocked_dependency)\b/im)?.[1]?.toLowerCase()??null;}
function split(out){return out?out.split(/\r?\n/).filter(Boolean).map(x=>x.replaceAll('\\','/')):[];}
function working(w){return [...new Set([...split(must('git',['-C',w,'diff','--name-only'])),...split(must('git',['-C',w,'diff','--cached','--name-only'])),...split(must('git',['-C',w,'ls-files','--others','--exclude-standard']))])].sort();}
function committed(w,a,b){return a===b?[]:split(must('git',['-C',w,'diff','--name-only',`${a}..${b}`]));}
function pythonGuard(temp){const g=path.join(temp,'no-python');mkdirSync(g);const cmd='@echo off\r\necho ERROR: Python is forbidden by the UCI Arena Spark relay. 1>&2\r\nexit /b 126\r\n';for(const n of ['python','python3','py','pip','pip3']){writeFileSync(path.join(g,`${n}.cmd`),cmd);writeFileSync(path.join(g,`${n}.exe`),'');}const ext=['.CMD','.BAT','.COM','.EXE',...(process.env.PATHEXT??'').split(';').map(x=>x.toUpperCase()).filter(Boolean)];return{...process.env,PATH:`${g}${path.delimiter}${process.env.PATH??''}`,PATHEXT:[...new Set(ext)].join(';')};}
function prompt(d,w){return `LOCAL RELAY BOUNDARY — authoritative for this run\n\nYou are the bounded Spark agent. Operate ONLY in this clean relay worktree:\n${w}\n\nNEVER USE PYTHON: no python, python3, py, pip/pip3, .py helpers, Python diagnostics, manipulation, inspection, or tests.\nDo not create/reset/clean/stash/delete/repurpose other worktrees. Do not commit, push, merge, mark ready, close issues, choose the next issue, or redesign architecture unless Sol explicitly authorizes it. Leave any successful bounded edits unstaged; the relay alone scope-checks, commits, and pushes. Do not use network operations unless the dispatch requires them. Follow AGENTS.md and active authority.\n\n--- BEGIN SOL DISPATCH ---\n${d.text}\n--- END SOL DISPATCH ---\n`;}
function post(repo,n,body){gh(['api','--method','POST',`repos/${repo}/issues/${n}/comments`,'--input','-'],JSON.stringify({body}));}
function bound(x,n){x=String(x??'');return x.length<=n?x:`${x.slice(0,n-80)}\n[relay truncated]`;}
function resultBody(m,w,code,report,stderr,o){return `${RESULT}\n${JSON.stringify({dispatch_id:m.dispatch_id,initial_head:m.expected_head,final_head:o.finalHead,report_status:o.reportStatus,push_status:o.pushStatus})}\n-->\n### Local Spark relay result\n\n- Dispatch: \`${m.dispatch_id}\`\n- Worktree: \`${w}\`\n- Codex exit: \`${code}\`\n- Push guard: \`${o.pushStatus}\`\n- Final HEAD: \`${o.finalHead}\`\n- Changed files: ${o.files.length?o.files.map(x=>`\`${x}\``).join(', '):'_none_'}\n\n\`\`\`text\n${bound(report,45000)}\n\`\`\`${stderr?.trim()?`\n\nCodex stderr:\n\`\`\`text\n${bound(stderr,5000)}\n\`\`\``:''}`;}
function finish(repo,n,w,m,code,report,stderr){
  const o={reportStatus:status(report),pushStatus:'not_attempted',finalHead:must('git',['-C',w,'rev-parse','HEAD']),files:[]};
  if(code!==0){o.pushStatus='codex_exit_nonzero';post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  if(o.finalHead!==m.expected_head){o.pushStatus='agent_created_commit';post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  o.files=working(w);
  const allowed=new Set(m.allowed_paths);
  const bad=o.files.filter(x=>!allowed.has(x));
  if(bad.length){o.pushStatus=`scope_violation:${bad.join(',')}`;post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  if(!o.files.length){o.pushStatus='no_changes';post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  if(!m.push_statuses.includes(o.reportStatus)){o.pushStatus=`report_status_not_pushable:${o.reportStatus??'missing'}`;post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  const dc=run('git',['-C',w,'diff','--check']);
  if(dc.status!==0){o.pushStatus='diff_check_failed';post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  must('git',['-C',w,'add','--',...o.files]);
  const sc=run('git',['-C',w,'diff','--cached','--check']);
  if(sc.status!==0){o.pushStatus='staged_diff_check_failed';post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  const cm=run('git',['-C',w,'commit','-m',`relay(${m.dispatch_id}): bounded Spark result`]);
  if(cm.status!==0){o.pushStatus=`commit_failed:${(cm.stderr||cm.stdout||'').trim()}`;post(repo,n,resultBody(m,w,code,report,stderr,o));return o;}
  o.finalHead=must('git',['-C',w,'rev-parse','HEAD']);
  o.files=committed(w,m.expected_head,o.finalHead);
  const bad2=o.files.filter(x=>!allowed.has(x));
  if(bad2.length)o.pushStatus=`committed_scope_violation:${bad2.join(',')}`;
  else if(must('git',['-C',w,'status','--porcelain']))o.pushStatus='worktree_not_clean_after_commit';
  else if(run('git',['-C',w,'merge-base','--is-ancestor',m.expected_head,o.finalHead]).status!==0)o.pushStatus='bad_commit_ancestry';
  else{
    verify(pr(repo,n),m);
    const pu=run('git',['-C',w,'push','origin',`HEAD:refs/heads/${m.target_branch}`]);
    if(pu.status!==0)o.pushStatus=`push_failed:${(pu.stderr||pu.stdout||'').trim()}`;
    else if(pr(repo,n).head?.sha!==o.finalHead)o.pushStatus='remote_verification_failed';
    else o.pushStatus='pushed_and_verified';
  }
  post(repo,n,resultBody(m,w,code,report,stderr,o));
  return o;
}

function processOne(c,d){const m=d.meta;let temp;try{verify(pr(c.repo,c.pr),m);const w=prepare(c.checkout,c.worktreeRoot,m);temp=mkdtempSync(path.join(tmpdir(),'uai-spark-relay-'));const out=path.join(temp,'last.txt');const ca=[];for(const x of c.addDirs)ca.push('--add-dir',x);ca.push('exec','-C',w,'--profile',c.profile,'--sandbox','workspace-write','--ephemeral','--output-last-message',out,'-');const r=run('codex',ca,{input:prompt(d,w),env:pythonGuard(temp)});const report=existsSync(out)?readFileSync(out,'utf8'):(r.stdout||'');const o=finish(c.repo,c.pr,w,m,r.status??1,report,r.stderr);console.log(`[relay] ${m.dispatch_id}: ${o.pushStatus}`);}catch(e){console.error(`[relay] ${m.dispatch_id} blocked: ${e.message}`);post(c.repo,c.pr,`${RESULT}\n${JSON.stringify({dispatch_id:m.dispatch_id,report_status:'relay_blocked',push_status:'not_attempted'})}\n-->\n### Local Spark relay blocked\n\n\`${bound(e.message,12000)}\``);}finally{if(temp)rmSync(temp,{recursive:true,force:true});}}
function selfTest(){const sha='a'.repeat(40),body=`${DISPATCH}\n${JSON.stringify({version:1,dispatch_id:'t',repo:'o/r',pr:1,expected_head:sha,target_branch:'b',allowed_paths:['x'],push_statuses:['completed']})}\n-->\nTask`;const d=marked(body,DISPATCH);validate(d,'o/r',1);if(d.text!=='Task'||status('STATUS: completed')!=='completed'||norm('.\\a\\b')!=='a/b')throw new Error('self-test failed');console.log('agent-relay self-test: PASS');}
function main(){const c=args(process.argv.slice(2));if(c.help){process.stdout.write(help());return;}if(c.selfTest){selfTest();return;}if(!c.repo||!/^[^/\s]+\/[^/\s]+$/.test(c.repo)||!Number.isInteger(c.pr)||c.pr<=0||!c.checkout)throw new Error('--repo, --pr, and --checkout are required');if(!Number.isFinite(c.poll)||c.poll<5)throw new Error('--poll-seconds must be >= 5');c.author=c.author??c.repo.split('/')[0];c.worktreeRoot=c.worktreeRoot??path.resolve(c.checkout,'..','uai-agent-relay-worktrees');must('gh',['auth','status']);must('codex',['--version']);must('git',['-C',c.checkout,'rev-parse','--show-toplevel']);console.log(`[relay] watching ${c.repo} PR #${c.pr}; author=${c.author}; profile=${c.profile}`);for(;;){const q=pending(commentPages(c.repo,c.pr),c.author,c.repo,c.pr);if(q.length){processOne(c,q[0]);if(c.once)return;}else if(c.once){console.log('[relay] no pending dispatch');return;}Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,c.poll*1000);}}
try{main();}catch(e){console.error(`[relay] fatal: ${e.message}`);process.exitCode=1;}
