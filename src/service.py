#!/usr/bin/env python3
"""Private stdin/stdout JSON transport for m pocket. Never logs credentials."""
import base64
import hashlib
import html
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from email.utils import parsedate_to_datetime

RELAY = 'https://pocket-mastodon-callback.davforge.workers.dev'
SCOPES = 'read:statuses write:statuses'
MAX_RESPONSE = 2 * 1024 * 1024

class Problem(Exception):
    def __init__(self, message, status=0, retry=0):
        super().__init__(message)
        self.status, self.retry = status, retry

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

def hostname(value):
    try:
        host = value.strip().lower().encode('idna').decode('ascii')
    except (ValueError, UnicodeError, AttributeError):
        raise Problem('Enter a valid server hostname.')
    if len(host)>253 or not re.fullmatch(r'(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}',host):
        raise Problem('Use a hostname, without https:// or a path.')
    return host

def retry_delay(value):
    try: return max(1, min(86400, int(value)))
    except (ValueError, TypeError):
        try: return max(1, min(86400, int(parsedate_to_datetime(value).timestamp()-time.time())))
        except (ValueError, TypeError, AttributeError): return 60

class Http:
    def __init__(self):
        self.opener=urllib.request.build_opener(NoRedirect(),urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    def request(self, url, method='GET', token='', form=None, payload=None):
        if not url.startswith('https://'): raise Problem('HTTPS is required.')
        headers={'Accept':'application/json','User-Agent':'m-pocket/0.2.0-beta1'}
        if token: headers['Authorization']='Bearer '+token
        body=None
        if form is not None:
            body=urllib.parse.urlencode(form).encode();headers['Content-Type']='application/x-www-form-urlencoded'
        if payload is not None:
            body=json.dumps(payload).encode();headers['Content-Type']='application/json'
        try:
            with self.opener.open(urllib.request.Request(url,data=body,headers=headers,method=method),timeout=15) as res:
                raw=res.read(MAX_RESPONSE+1)
                if len(raw)>MAX_RESPONSE: raise Problem('Server response is too large.')
                return res.status,json.loads(raw)
        except urllib.error.HTTPError as error:
            delay=retry_delay(error.headers.get('Retry-After')) if error.code==429 else 0
            raise Problem('Too many requests. Wait and retry.' if error.code==429 else 'Server returned HTTP '+str(error.code)+'.',error.code,delay)
        except (urllib.error.URLError,TimeoutError,OSError): raise Problem('Connection failed. Check Wi-Fi and try again.')
        except (ValueError, UnicodeError): raise Problem('Invalid server response.')

class PlainText(HTMLParser):
    def __init__(self):super().__init__(convert_charrefs=True);self.parts=[]
    def handle_starttag(self,tag,attrs):
        if tag=='br':self.parts.append('\n')
    def handle_endtag(self,tag):
        if tag in ('p','li','blockquote'):self.parts.append('\n')
    def handle_data(self,data):self.parts.append(data)
    @classmethod
    def convert(cls,value):
        p=cls();p.feed(value if isinstance(value,str) else '');return ''.join(p.parts).strip()

def posts_from(data):
    if not isinstance(data,list):raise Problem('Invalid timeline response.')
    result=[]
    for original in data:
        if not isinstance(original,dict) or not isinstance(original.get('id'),str):continue
        post=original.get('reblog') if isinstance(original.get('reblog'),dict) else original
        account=post.get('account') if isinstance(post.get('account'),dict) else {}
        acct=account.get('acct') if isinstance(account.get('acct'),str) else ''
        name=account.get('display_name') or acct
        if not isinstance(name,str):name=acct
        text=PlainText.convert(post.get('content'))
        warning=post.get('spoiler_text')
        if isinstance(warning,str) and warning:text='CW: '+warning+'\n'+text
        if post.get('media_attachments'):text+='\n[Media attached]'
        result.append({'id':original['id'],'title':name+' @'+acct,'body':text[:8192] + ('\n[Long post truncated]' if len(text)>8192 else '')})
    return result

class Client:
    def __init__(self, config_dir=None, http=None):
        self.directory=Path(config_dir or os.environ.get('XDG_CONFIG_HOME',str(Path.home()/'.config')))/'m-pocket'
        self.directory.mkdir(mode=0o700,parents=True,exist_ok=True)
        self.directory.chmod(0o700)
        self.http=http or Http();self.token='';self.auth=None;self.retry_at=0;self.poll_failures=0
        self.prefs=self.load('preferences.json',{'dark':False,'large':False,'server':'mastodon.social'})
        try:self.server=hostname(self.prefs.get('server','mastodon.social'))
        except Problem:self.server='mastodon.social'
    def load(self,name,default):
        try:
            value=json.loads((self.directory/name).read_text())
            return value if isinstance(value,dict) else default
        except (OSError,ValueError):return default
    def save(self,name,value):
        fd,path=tempfile.mkstemp(prefix='.write-',dir=self.directory)
        try:
            with os.fdopen(fd,'w') as stream:json.dump(value,stream);stream.flush();os.fsync(stream.fileno())
            os.replace(path,self.directory/name)
        finally:
            if os.path.exists(path):os.unlink(path)
    def mastodon(self,path,**kwargs):
        if time.monotonic()<self.retry_at:raise Problem('Wait '+str(int(self.retry_at-time.monotonic())+1)+'s before retrying.')
        try:return self.http.request('https://'+self.server+path,**kwargs)
        except Problem as error:
            if error.status==429:self.retry_at=time.monotonic()+error.retry
            raise
    def cancel(self):
        auth,self.auth=self.auth,None
        if auth and auth.get('poll_secret'):
            try:self.http.request(RELAY+'/sessions/'+auth['state'],method='DELETE',token=auth['poll_secret'])
            except Problem:pass
        self.poll_failures=0
    def login(self):
        self.cancel()
        _,metadata=self.mastodon('/.well-known/oauth-authorization-server')
        if not isinstance(metadata,dict) or 'S256' not in metadata.get('code_challenge_methods_supported',[]):
            raise Problem('Server needs PKCE S256. Use an access token.')
        redirect=RELAY+'/callback';key=self.server+'|'+redirect
        registry=self.load('registrations.json',{});registration=registry.get(key)
        if not isinstance(registration,dict) or not all(isinstance(registration.get(k),str) and registration[k] for k in ('client_id','client_secret')):
            _,registration=self.mastodon('/api/v1/apps',method='POST',form={'client_name':'m pocket','redirect_uris':redirect,'scopes':SCOPES})
            if not isinstance(registration,dict) or not all(isinstance(registration.get(k),str) and registration[k] for k in ('client_id','client_secret')):raise Problem('Could not register the application.')
            registry[key]={k:registration[k] for k in ('client_id','client_secret')};self.save('registrations.json',registry)
        verifier=secrets.token_hex(32);state=secrets.token_hex(32)
        challenge=base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
        try:status,pairing=self.http.request(RELAY+'/sessions',method='POST',payload={'state':state,'server':self.server,'client_id':registration['client_id'],'challenge':challenge})
        except Problem: return {'fallback':True,'message':'QR service unavailable. Use an access token.'}
        if status!=201 or not isinstance(pairing,dict) or not isinstance(pairing.get('poll_secret'),str):return {'fallback':True,'message':'QR service unavailable. Use an access token.'}
        self.auth={**registration,'verifier':verifier,'state':state,'poll_secret':pairing['poll_secret'],'expires':time.monotonic()+600}
        return {'qr':RELAY+'/s/'+state,'interval':3}
    def poll(self):
        auth=self.auth
        if not auth or time.monotonic()>auth['expires']:
            self.cancel();return {'fallback':True,'message':'Sign-in expired. Retry or use a token.'}
        try:
            status,value=self.http.request(RELAY+'/sessions/'+auth['state'],token=auth['poll_secret'])
        except Problem:
            self.poll_failures+=1
            if self.poll_failures>=3:self.cancel();return {'fallback':True,'message':'QR service unavailable. Use a token.'}
            return {'pending':True,'interval':10}
        self.poll_failures=0
        if status==202:return {'pending':True,'interval':3}
        if not isinstance(value,dict) or value.get('state')!=auth['state']:
            self.cancel();raise Problem('Invalid callback. Please sign in again.')
        if value.get('error'):
            self.cancel();raise Problem('Access was not approved.')
        code=value.get('code')
        if not isinstance(code,str) or not code or len(code)>2048:self.cancel();raise Problem('Invalid authorization response.')
        try:
            _,response=self.mastodon('/oauth/token',method='POST',form={'grant_type':'authorization_code','client_id':auth['client_id'],'client_secret':auth['client_secret'],'redirect_uri':RELAY+'/callback','code':code,'code_verifier':auth['verifier']})
            candidate=response.get('access_token') if isinstance(response,dict) else None
            if not isinstance(candidate,str) or not candidate:raise Problem('Server did not return an access token.')
            self.token=candidate
        finally:self.cancel()
        return {'authenticated':True}
    def manual(self,value):
        candidate=value.strip() if isinstance(value,str) else ''
        if not re.fullmatch(r'[A-Za-z0-9._~+/=-]{1,4096}',candidate):raise Problem('Enter a valid access token.')
        _,data=self.mastodon('/api/v1/timelines/home?limit=1',token=candidate)
        posts_from(data)
        self.token=candidate
        return {'authenticated':True}
    def handle(self,command):
        op=command.get('op')
        if op=='prefs':return {'prefs':{**self.prefs,'server':self.server}}
        if op=='save_prefs':
            self.prefs.update(dark=bool(command.get('dark')),large=bool(command.get('large')),server=self.server);self.save('preferences.json',self.prefs);return {}
        if op=='server':
            host=hostname(command.get('server',''));self.cancel();self.server=host;self.token='';self.retry_at=0;self.prefs['server']=host;self.save('preferences.json',self.prefs);return {'server':host}
        if op=='login':return self.login()
        if op=='poll':return self.poll()
        if op=='cancel':self.cancel();return {}
        if op=='token':return self.manual(command.get('token',''))
        if op=='logout':self.cancel();self.token='';return {'signed_out':True}
        if op=='feed':
            home=bool(command.get('home',True))
            if home and not self.token:raise Problem('Sign in to read your home feed.')
            query={'limit':'20'}
            if not home:query['local']='true'
            if command.get('max_id'):
                cursor=command['max_id']
                if not isinstance(cursor,str) or not re.fullmatch(r'[0-9]{1,40}',cursor):raise Problem('Invalid feed cursor.')
                query['max_id']=cursor
            _,data=self.mastodon('/api/v1/timelines/'+('home' if home else 'public')+'?'+urllib.parse.urlencode(query),token=self.token)
            return {'posts':posts_from(data)}
        if op=='publish':
            if not self.token:raise Problem('Sign in before posting.')
            text=command.get('text','')
            if not isinstance(text,str) or not text.strip() or len(text)>500:raise Problem('Use 1-500 characters.')
            self.mastodon('/api/v1/statuses',method='POST',token=self.token,form={'status':text,'visibility':'public'})
            return {'published':True}
        raise Problem('Unknown operation.')

def main():
    client=Client()
    for line in sys.stdin:
        request_id=None
        try:
            if len(line)>32768:raise Problem('Request too large.')
            command=json.loads(line)
            if not isinstance(command,dict):raise Problem('Invalid request.')
            request_id=command.get('id');result=client.handle(command)
            result.update(id=request_id,ok=True)
        except Problem as error:result={'id':request_id,'ok':False,'error':str(error),'status':error.status,'retry':error.retry}
        except Exception:result={'id':request_id,'ok':False,'error':'Could not complete the request.'}
        print(json.dumps(result,ensure_ascii=True),flush=True)

if __name__=='__main__':main()
