export const SIGSTORE_BUNDLE_MEDIA_TYPE = "application/vnd.dev.sigstore.bundle.v0.3+json";
const INTOTO = "https://in-toto.io/Statement/v1";
function fail(message) { throw new Error(`invalid Sigstore bundle: ${message}`); }
function exact(value, allowed, label) { if (!value || typeof value !== "object" || Array.isArray(value)) fail(label); const extra=Object.keys(value).filter(key=>!allowed.includes(key)); if(extra.length)fail(`${label} fields`); }
function canonicalBase64(value,label){if(typeof value!=="string"||value.length===0||value.length%4!==0||!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value))fail(label); const decoded=Buffer.from(value,"base64");if(decoded.toString("base64")!==value)fail(label);return decoded;}
function freeze(value){if(value&&typeof value==="object"&&!Buffer.isBuffer(value)){for(const child of Object.values(value))freeze(child);Object.freeze(value);}return value;}

export function parseDsseBundle({ bytes, maximumBytes }) {
  if(!Buffer.isBuffer(bytes)&&!(bytes instanceof Uint8Array))fail("bytes");
  if(!Number.isSafeInteger(maximumBytes)||maximumBytes<1||bytes.byteLength>maximumBytes)fail("size");
  let bundle; try{bundle=JSON.parse(Buffer.from(bytes).toString("utf8"));}catch{fail("JSON");}
  exact(bundle,["mediaType","verificationMaterial","dsseEnvelope"],"bundle");
  if(bundle.mediaType!==SIGSTORE_BUNDLE_MEDIA_TYPE||"messageSignature" in bundle)fail("media type");
  const envelope=bundle.dsseEnvelope; exact(envelope,["payloadType","payload","signatures"],"DSSE envelope");
  if(envelope.payloadType!=="application/vnd.in-toto+json"||!Array.isArray(envelope.signatures)||envelope.signatures.length!==1)fail("DSSE envelope");
  exact(envelope.signatures[0],["sig","keyid"],"DSSE signature"); canonicalBase64(envelope.signatures[0].sig,"signature base64");
  const payload=canonicalBase64(envelope.payload,"payload base64"); let statement;try{statement=JSON.parse(payload.toString("utf8"));}catch{fail("statement JSON");}
  if(statement?._type!==INTOTO||!Array.isArray(statement.subject)||statement.subject.length!==1||typeof statement.predicateType!=="string"||!("predicate" in statement))fail("statement");
  const result={bundle,envelope,statement,predicate:statement.predicate,verificationMaterial:bundle.verificationMaterial,signatureCount:1}; return freeze(structuredClone(result));
}
