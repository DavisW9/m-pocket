import assert from 'node:assert/strict';
const base=process.argv[2] || 'http://127.0.0.1:8793';
const state=[...crypto.getRandomValues(new Uint8Array(32))].map(n=>n.toString(16).padStart(2,'0')).join('');
const verifier='integration-verifier-'+state;
const challenge=Buffer.from(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(verifier))).toString('base64url');
let secret;
const req=(path,options={})=>fetch(base+path,{redirect:'manual',...options});
try {
 assert.equal((await req('/health')).status,200);
 const create=await req('/sessions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({state,server:'mastodon.social',client_id:'synthetic-test-client',challenge})});
 assert.equal(create.status,201,await create.clone().text());secret=(await create.json()).poll_secret;
 const authorization={Authorization:'Bearer '+secret};
 assert.equal((await req('/sessions/'+state)).status,401);
 assert.equal((await req('/sessions/'+state,{headers:authorization})).status,202);
 const landing=await req('/s/'+state);assert.match(await landing.text(),/Continue to mastodon.social/);
 const redirect=await req('/s/'+state+'/authorize');assert.equal(redirect.status,302);const url=new URL(redirect.headers.get('Location'));assert.equal(url.searchParams.get('code_challenge'),challenge);assert.equal(url.searchParams.get('redirect_uri'),base+'/callback');
 const callbacks=await Promise.all(['synthetic-code-1','synthetic-code-2'].map(code=>req('/callback?state='+state+'&code='+code)));
 assert.ok(callbacks.every(r=>r.status===200));
 await new Promise(r=>setTimeout(r,3100));
 const result=await req('/sessions/'+state,{headers:authorization});assert.equal(result.status,200);const body=await result.json();assert.ok(['synthetic-code-1','synthetic-code-2'].includes(body.code));assert.equal(body.state,state);
 await new Promise(r=>setTimeout(r,3100));
 assert.equal((await (await req('/sessions/'+state,{headers:authorization})).json()).code,body.code);
 assert.equal((await req('/sessions/'+state,{method:'DELETE',headers:authorization})).status,200);
 assert.equal((await req('/sessions/'+state,{headers:authorization})).status,410);
 console.log('PASS: deployed HTTP routes, Durable Object RPC/storage, private polling, PKCE URL, concurrent callback, retry delivery, cancellation');
} finally {if(secret)await req('/sessions/'+state,{method:'DELETE',headers:{Authorization:'Bearer '+secret}});}
