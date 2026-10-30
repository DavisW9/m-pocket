import { DurableObject } from 'cloudflare:workers';
import handler, { PairingCore } from './protocol.mjs';
export default handler;
export class Pairing extends DurableObject {
  constructor(ctx,env) {super(ctx,env);this.core=new PairingCore(ctx);}
  async request(request) {return this.core.request(request);}
  async alarm() {return this.core.alarm();}
}
