// No passwords, client secrets, PKCE verifiers, or access tokens reach this service.
const TTL = 10 * 60 * 1000;
const hex64 = /^[a-f0-9]{64}$/;
const headers = {'Cache-Control':'no-store','Referrer-Policy':'no-referrer','X-Content-Type-Options':'nosniff','Content-Security-Policy':"default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"};
const json = (data,status=200,extra={}) => Response.json(data,{status,headers:{...headers,...extra}});
const escape = s => s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const page = (text,link='',label='') => new Response(`<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Pocket Mastodon</title><style>body{font:18px system-ui;background:#f3effa;color:#302740;max-width:32rem;margin:12vh auto;padding:24px;line-height:1.6}a{display:block;background:#7054aa;color:white;padding:12px 18px;text-decoration:none;border-radius:8px}small{color:#726585}</style><h1>Pocket Mastodon</h1><p>${escape(text)}</p>${link?`<a href="${escape(link)}">${escape(label)}</a>`:''}<p><small>You can close this page after approving access.</small></p></html>`,{headers:{...headers,'Content-Type':'text/html; charset=utf-8'}});
const digest = async s => [...new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(s)))].map(n=>n.toString(16).padStart(2,'0')).join('');
const random = () => [...crypto.getRandomValues(new Uint8Array(32))].map(n=>n.toString(16).padStart(2,'0')).join('');

export default {
  async fetch(request,env) {
    const url=new URL(request.url);
    if(url.pathname==='/health') return json({ok:true});
    if(url.pathname==='/sessions' && request.method==='POST') {
      // The platform sets this header; no IP addresses are persisted.
      const key=await digest(request.headers.get('CF-Connecting-IP') || 'local');
      const limiter=env.PAIRINGS.getByName('rate:'+key);
      const allowed=await limiter.request(new Request(url.origin+'/rate',{method:'POST'}));
      if(allowed.status!==200)return allowed;
      let data;
      try {const text=await request.text();if(text.length>4096)return json({error:'too_large'},413);data=JSON.parse(text);}catch{return json({error:'invalid_json'},400);}
      if(!data || !hex64.test(data.state) || typeof data.server!=='string' || data.server.length>253 || !/^(?=.{1,253}$)(?:[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$/.test(data.server) || typeof data.client_id!=='string' || !/^[\w-]{1,256}$/.test(data.client_id) || !/^[\w-]{43}$/.test(data.challenge))return json({error:'invalid_session'},400);
      return env.PAIRINGS.getByName(data.state).request(new Request(url.origin+'/create',{method:'POST',body:JSON.stringify(data)}));
    }
    const match=url.pathname.match(/^\/(?:s|sessions)\/([a-f0-9]{64})(?:\/authorize)?$/);
    const id=match?.[1] || (url.pathname==='/callback'?url.searchParams.get('state'):null);
    if(!id || !hex64.test(id))return json({error:'not_found'},404);
    return env.PAIRINGS.getByName(id).request(request);
  }
};

export class PairingCore {
  constructor(ctx) {this.ctx=ctx;this.storage=ctx.storage;}
  async alarm() {await this.storage.deleteAll();}
  async request(request) {return this.handle(request);}
  async handle(request) {
    const url=new URL(request.url),now=Date.now();
    if(url.pathname==='/rate') {
      let rate=this.storage.kv.get('rate');
      if(!rate || rate.until<=now)rate={count:0,until:now+TTL};
      if(rate.count>=10)return json({error:'rate_limited'},429,{'Retry-After':'600'});
      rate.count++;this.storage.kv.put('rate',rate);await this.storage.setAlarm(rate.until);return json({ok:true});
    }
    if(url.pathname==='/create' && request.method==='POST') {
      const data=await request.json(),secret=random(),pollHash=await digest(secret);
      if(this.storage.kv.get('session'))return json({error:'exists'},409);
      this.storage.kv.put('session',{...data,pollHash,expires:now+TTL,lastPoll:0,result:null});
      await this.storage.setAlarm(now+TTL);
      return json({poll_secret:secret,expires_in:600,interval:3},201);
    }
    const secret=request.headers.get('Authorization')?.replace(/^Bearer /,'') || '';
    const secretHash=hex64.test(secret)?await digest(secret):'';
    const s=this.storage.kv.get('session');
    if(!s || s.expires<=now)return json({error:'expired'},410);
    if(url.pathname.startsWith('/sessions/')) {
      if(!secretHash || secretHash!==s.pollHash)return json({error:'unauthorized'},401);
      if(request.method==='DELETE') {this.storage.kv.put('session',{expires:0});return json({ok:true});}
      if(request.method!=='GET')return json({error:'method'},405);
      if(now-s.lastPoll<2000)return json({error:'slow_down'},429,{'Retry-After':'3'});
      s.lastPoll=now;this.storage.kv.put('session',s);
      // Idempotent delivery: a dropped response cannot strand the device.
      // Device DELETEs after exchange; OAuth codes themselves are single-use.
      return s.result?json({...s.result,state:s.state}):json({status:'pending'},202);
    }
    if(request.method!=='GET')return json({error:'method'},405);
    if(url.pathname==='/callback') {
      if([...url.searchParams.keys()].some(k=>url.searchParams.getAll(k).length!==1))return json({error:'duplicate_parameter'},400);
      if(url.searchParams.get('state')!==s.state)return json({error:'state'},400);
      const code=url.searchParams.get('code'),error=url.searchParams.get('error');
      if(s.result)return page('This sign-in link has already been used. Return to Pocket Mastodon.');
      if(error) s.result={error:'access_denied'};
      else if(code && code.length<=2048)s.result={code};
      else return json({error:'missing_code'},400);
      this.storage.kv.put('session',s);
      return page(error?'Access was not approved. Return to Pocket Mastodon to try again.':'Approval received. Your Cardputer will finish signing in automatically. No code to type.');
    }
    if(s.result)return page('Approval already received. Return to Pocket Mastodon.');
    const auth=new URL('https://'+s.server+'/oauth/authorize');
    auth.search=new URLSearchParams({response_type:'code',client_id:s.client_id,redirect_uri:url.origin+'/callback',scope:'read:statuses write:statuses',state:s.state,code_challenge:s.challenge,code_challenge_method:'S256'});
    if(url.pathname.endsWith('/authorize'))return new Response(null,{status:302,headers:{...headers,Location:auth.href}});
    return page('Sign in to '+s.server+' to connect the Cardputer that displayed this QR. Only continue if you started this sign-in.',url.pathname+'/authorize','Continue to '+s.server);
  }
}
