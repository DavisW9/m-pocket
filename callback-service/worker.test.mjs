import test from 'node:test';
import assert from 'node:assert/strict';
import worker,{PairingCore as Pairing} from './protocol.mjs';
function environment(){
 const objects=new Map();
 return {PAIRINGS:{getByName(n){if(!objects.has(n)){
  const values=new Map();const storage={kv:{get:k=>structuredClone(values.get(k)),put:(k,v)=>values.set(k,structuredClone(v))},get:async k=>structuredClone(values.get(k)),put:async(k,v)=>values.set(k,structuredClone(v)),setAlarm:async()=>{},deleteAll:async()=>values.clear()};
  objects.set(n,new Pairing({storage,blockConcurrencyWhile:f=>f()}));
 }return objects.get(n);}},objects};
}
const origin='https://pocket.example';
const state='a'.repeat(64);
const payload={state,server:'mastodon.social',client_id:'client-id',challenge:'x'.repeat(43)};
const request=(path,method='GET',body,secret)=>new Request(origin+path,{method,headers:{...(body?{'Content-Type':'application/json'}:{}),...(secret?{Authorization:'Bearer '+secret}:{})},body:body?JSON.stringify(body):undefined});
async function create(env){const res=await worker.fetch(request('/sessions','POST',payload),env);assert.equal(res.status,201);return (await res.json()).poll_secret;}
test('phone approval, secret-isolated polling, PKCE and acknowledgement',async()=>{
 const env=environment(),secret=await create(env);
 const page=await worker.fetch(request('/s/'+state),env);assert.match(await page.text(),/Continue to mastodon.social/);
 const auth=await worker.fetch(request('/s/'+state+'/authorize'),env);const url=new URL(auth.headers.get('Location'));
 assert.equal(url.origin,'https://mastodon.social');assert.equal(url.searchParams.get('state'),state);assert.equal(url.searchParams.get('code_challenge_method'),'S256');assert.equal(url.searchParams.get('redirect_uri'),origin+'/callback');assert.equal(url.searchParams.get('code_challenge'),payload.challenge);assert.ok(!url.href.includes(secret));
 assert.equal((await worker.fetch(request('/sessions/'+state),env)).status,401);
 assert.equal((await worker.fetch(request('/sessions/'+state,'GET',null,'b'.repeat(64)),env)).status,401);
 assert.equal((await worker.fetch(request('/callback?state='+state+'&code=fake-code'),env)).status,200);
 const result=await worker.fetch(request('/sessions/'+state,'GET',null,secret),env);assert.deepEqual(await result.json(),{code:'fake-code',state});assert.equal(result.headers.get('Cache-Control'),'no-store');
 await worker.fetch(request('/callback?state='+state+'&code=overwrite'),env);
 const object=env.objects.get(state);const s=await object.storage.get('session');assert.equal(s.result.code,'fake-code');s.lastPoll=0;await object.storage.put('session',s);
 assert.equal((await (await worker.fetch(request('/sessions/'+state,'GET',null,secret),env)).json()).code,'fake-code');
 assert.equal((await worker.fetch(request('/sessions/'+state,'DELETE',null,secret),env)).status,200);
 assert.equal((await worker.fetch(request('/sessions/'+state,'GET',null,secret),env)).status,410);
});
test('pending, duplicate state, invalid state, denial, expiry and poll backoff',async()=>{
 const env=environment(),secret=await create(env);
 assert.equal((await worker.fetch(request('/sessions/'+state,'GET',null,secret),env)).status,202);
 assert.equal((await worker.fetch(request('/sessions/'+state,'GET',null,secret),env)).status,429);
 assert.equal((await worker.fetch(request('/callback?state='+state+'&state='+state+'&code=x'),env)).status,400);
 assert.equal((await worker.fetch(request('/callback?state='+'b'.repeat(64)+'&code=x'),env)).status,410);
 await worker.fetch(request('/callback?state='+state+'&error=access_denied'),env);
 const obj=env.objects.get(state);let s=await obj.storage.get('session');assert.equal(s.result.error,'access_denied');s.expires=0;await obj.storage.put('session',s);
 assert.equal((await worker.fetch(request('/s/'+state),env)).status,410);
 await obj.alarm();assert.equal(await obj.storage.get('session'),undefined);
});
test('rejects malformed sessions, HTML injection and session overwrite',async()=>{
 const env=environment();await create(env);
 assert.equal((await worker.fetch(request('/sessions','POST',payload),env)).status,409);
 for(const changed of [{server:'evil.test/<script>'},{server:'localhost'},{challenge:'x'},{state:'short'},{client_id:'<script>'}])assert.equal((await worker.fetch(request('/sessions','POST',{...payload,...changed}),env)).status,400);
 assert.equal((await worker.fetch(new Request(origin+'/sessions',{method:'POST',body:'null'}),env)).status,400);
});
test('creation rate limit protects the free quota',async()=>{
 const env=environment();for(let i=0;i<10;i++)await worker.fetch(request('/sessions','POST',{}),env);
 const response=await worker.fetch(request('/sessions','POST',payload),env);assert.equal(response.status,429);assert.equal(response.headers.get('Retry-After'),'600');
});
