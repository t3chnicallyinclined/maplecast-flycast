// texture-manager.mjs — Dreamcast texture decode + WebGPU texture cache
// OPTIMIZED: dirty-page-aware caching, texture reuse, no per-frame GPU alloc

const detwiddle = [new Array(11), new Array(11)];
(() => {
    function tw(x, y, xs, ys) {
        let r=0, s=0; xs>>=1; ys>>=1;
        while (xs||ys) { if(ys){r|=(y&1)<<s;ys>>=1;y>>=1;s++;} if(xs){r|=(x&1)<<s;xs>>=1;x>>=1;s++;} }
        return r;
    }
    for (let s=0;s<11;s++) { detwiddle[0][s]=new Uint32Array(1024); detwiddle[1][s]=new Uint32Array(1024);
        const ys=1<<s; for(let i=0;i<1024;i++){detwiddle[0][s][i]=tw(i,0,1024,ys);detwiddle[1][s][i]=tw(0,i,ys,1024);} }
})();
function twop(x,y,bx,by){return detwiddle[0][by][x]+detwiddle[1][bx][y];}
function bsr(v){let r=0;while((1<<r)<v)r++;return r;}
function u1555(c){return[(((c>>10)&31)*255/31)|0,(((c>>5)&31)*255/31)|0,((c&31)*255/31)|0,(c>>15)?255:0];}
function u565(c){return[(((c>>11)&31)*255/31)|0,(((c>>5)&63)*255/63)|0,((c&31)*255/31)|0,255];}
function u4444(c){return[(((c>>8)&15)*255/15)|0,(((c>>4)&15)*255/15)|0,((c&15)*255/15)|0,(((c>>12)&15)*255/15)|0];}

const PAGE_SIZE = 4096;

export class TextureManager {
    constructor(device) {
        this.device = device;
        this.cache = new Map();      // genKey → {texture, sampler, w, h}
        this._prevCache = new Map(); // baseKey → {texture, sampler, w, h} (for GPU texture reuse)
        this.gen = 0;
        this._pal = null;
        this._fb = null;
        this._lastPvrRegs = null;
        this.stats = { hits: 0, misses: 0, reused: 0 };
    }

    // Mark all textures as needing re-decode (bumps generation)
    // Previous gen's GPUTextures are kept in _prevCache for reuse
    invalidateAll() {
        this.gen++;
        this.cache.clear(); // Clear gen-keyed cache (forces re-decode on next access)
    }

    updatePalette(regs) {
        if (!regs || regs.length < 0x1000+4096) return;
        const pv = new DataView(regs.buffer, regs.byteOffset+0x1000, 4096);
        const ctrl = new DataView(regs.buffer, regs.byteOffset).getUint32(0x108,true) & 3;
        this._pal = new Uint8Array(4096);
        const unp = [u1555,u565,u4444,u4444][ctrl];
        for (let i=0;i<1024;i++) {
            const raw = pv.getUint32(i*4,true);
            const c = ctrl===3 ? [(raw>>16)&0xFF,(raw>>8)&0xFF,raw&0xFF,(raw>>24)&0xFF] : unp(raw&0xFFFF);
            this._pal[i*4]=c[0]; this._pal[i*4+1]=c[1]; this._pal[i*4+2]=c[2]; this._pal[i*4+3]=c[3];
        }
        this._palDirty = false;
    }

    getTexture(tsp, tcw, vram) {
        const addr=(tcw&0x1FFFFF)<<3, fmt=(tcw>>27)&7, texU=(tsp>>3)&7, texV=tsp&7;
        const w=8<<texU, h=8<<texV, palSel=(tcw>>21)&0x3F, scan=(tcw>>26)&1;

        // Two-level cache: base key (stable) + gen key (current generation)
        const baseKey = `${addr}_${fmt}_${texU}_${texV}_${palSel}`;
        const genKey = `${baseKey}_${this.gen}`;

        // Fast path: current generation cache hit (no decode needed)
        const genCached = this.cache.get(genKey);
        if (genCached) { this.stats.hits++; return genCached; }

        // Check if we have a previous generation's GPUTexture to reuse
        const prevEntry = this._prevCache.get(baseKey);
        const rgba = this._decode(vram, addr, fmt, w, h, palSel, scan);
        if (!rgba) return this.getFallbackTexture();

        let texture, sampler;
        if (prevEntry && prevEntry.w === w && prevEntry.h === h) {
            // REUSE existing GPUTexture — just update its data (no createTexture!)
            texture = prevEntry.texture;
            sampler = prevEntry.sampler;
            this.device.queue.writeTexture({texture}, rgba, {bytesPerRow: w*4}, [w, h]);
            this.stats.reused++;
        } else {
            // Create new GPUTexture
            texture = this.device.createTexture({size:[w,h],format:'rgba8unorm',usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST});
            this.device.queue.writeTexture({texture},rgba,{bytesPerRow:w*4},[w,h]);
            const fm=(tsp>>13)&3, cu=(tsp>>16)&1, cv=(tsp>>15)&1, fu=(tsp>>18)&1, fv=(tsp>>17)&1;
            sampler = this.device.createSampler({minFilter:fm?'linear':'nearest',magFilter:fm?'linear':'nearest',
                addressModeU:cu?'clamp-to-edge':fu?'mirror-repeat':'repeat',addressModeV:cv?'clamp-to-edge':fv?'mirror-repeat':'repeat'});
            // Destroy old texture if it exists
            if (prevEntry) prevEntry.texture.destroy();
            this.stats.misses++;
        }

        const entry = {texture, sampler, w, h};
        this.cache.set(genKey, entry);
        this._prevCache.set(baseKey, entry);
        return entry;
    }

    _decode(vram,addr,fmt,w,h,palSel,scan) {
        const rgba=new Uint8Array(w*h*4), bx=bsr(w), by=bsr(h);
        if (fmt===5) return this._pal4(vram,addr,w,h,bx,by,palSel,rgba);
        if (fmt===6) return this._pal8(vram,addr,w,h,bx,by,palSel,rgba);
        const unp = fmt===0?u1555:fmt===1?u565:fmt===2?u4444:null;
        if (!unp) return null;
        for (let y=0;y<h;y++) for (let x=0;x<w;x++) {
            const idx = scan===1 ? y*w+x : twop(x,y,bx,by);
            const so = addr+idx*2; if(so+1>=vram.length)continue;
            const c=unp(vram[so]|(vram[so+1]<<8)), d=(y*w+x)*4;
            rgba[d]=c[0];rgba[d+1]=c[1];rgba[d+2]=c[2];rgba[d+3]=c[3];
        }
        return rgba;
    }

    _pal4(vram,addr,w,h,bx,by,palSel,rgba) {
        if(!this._pal)return null; const pb=palSel<<4;
        for(let y=0;y<h;y++)for(let x=0;x<w;x++){
            const ti=twop(x,y,bx,by),bo=addr+(ti>>1); if(bo>=vram.length)continue;
            const ni=(ti&1)?((vram[bo]>>4)&0xF):(vram[bo]&0xF), pi=(pb+ni)*4, d=(y*w+x)*4;
            rgba[d]=this._pal[pi];rgba[d+1]=this._pal[pi+1];rgba[d+2]=this._pal[pi+2];rgba[d+3]=this._pal[pi+3];
        } return rgba;
    }

    _pal8(vram,addr,w,h,bx,by,palSel,rgba) {
        if(!this._pal)return null; const pb=((palSel>>4)<<8);
        for(let y=0;y<h;y++)for(let x=0;x<w;x++){
            const ti=twop(x,y,bx,by),bo=addr+ti; if(bo>=vram.length)continue;
            const pi=(pb+vram[bo])*4, d=(y*w+x)*4;
            rgba[d]=this._pal[pi];rgba[d+1]=this._pal[pi+1];rgba[d+2]=this._pal[pi+2];rgba[d+3]=this._pal[pi+3];
        } return rgba;
    }

    getFallbackTexture() {
        if(this._fb)return this._fb;
        const t=this.device.createTexture({size:[1,1],format:'rgba8unorm',usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST});
        this.device.queue.writeTexture({texture:t},new Uint8Array([255,255,255,255]),{bytesPerRow:4},[1,1]);
        this._fb={texture:t,sampler:this.device.createSampler({minFilter:'nearest',magFilter:'nearest'})};
        return this._fb;
    }
}
