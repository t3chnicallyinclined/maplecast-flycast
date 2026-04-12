// pvr2-renderer.mjs — WebGPU renderer for PVR2 TA stream

import { vertexShader, fragmentShader } from './shaders.mjs';

const SBM=['zero','one','dst','one-minus-dst','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha'];
const DBM=['zero','one','src','one-minus-src','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha'];
const DCM=['never','less','equal','less-equal','greater','not-equal','greater-equal','always'];
const CM=['none','none','front','back'];

const VBL = { arrayStride: 28, attributes: [
    { shaderLocation:0, offset:0, format:'float32x3' },
    { shaderLocation:1, offset:12, format:'unorm8x4' },
    { shaderLocation:2, offset:16, format:'unorm8x4' },
    { shaderLocation:3, offset:20, format:'float32x2' },
]};

export class PVR2Renderer {
    constructor() {
        this.dev=null; this.ctx=null; this.fmt=null;
        this.pipes=new Map(); this.texBGs=new Map();
        this.uBuf=null; this.fBuf=null; this.vBuf=null; this.vBufSz=0;
        this.depth=null; this.bgl0=null; this.bgl1=null; this.pipeLayout=null;
        this.shader=null; this.uBG=null;
        // Dynamic frag uniform: 256-byte aligned slots, pre-allocated
        this.SLOT=256; this.MAX_SLOTS=8192;
        this.dynBuf=null; this.staging=null; this.stagingDV=null;
    }

    async init(canvas) {
        if (!navigator.gpu) throw new Error('WebGPU not supported');
        const a = await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
        if (!a) throw new Error('No WebGPU adapter');
        this.dev = await a.requestDevice();
        this.ctx = canvas.getContext('webgpu');
        this.fmt = navigator.gpu.getPreferredCanvasFormat();
        this.ctx.configure({device:this.dev,format:this.fmt,alphaMode:'opaque'});
        this._init(canvas.width, canvas.height);
        return this.dev;
    }

    _init(w,h) {
        const d = this.dev;
        this.uBuf = d.createBuffer({size:64,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
        this.dynBuf = d.createBuffer({size:this.SLOT*this.MAX_SLOTS,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
        this.staging = new ArrayBuffer(this.SLOT*this.MAX_SLOTS);
        this.stagingDV = new DataView(this.staging);
        this.depth = d.createTexture({size:[w,h],format:'depth32float',usage:GPUTextureUsage.RENDER_ATTACHMENT});
        this.bgl0 = d.createBindGroupLayout({entries:[
            {binding:0,visibility:GPUShaderStage.VERTEX,buffer:{type:'uniform'}},
            {binding:1,visibility:GPUShaderStage.FRAGMENT,buffer:{type:'uniform',hasDynamicOffset:true}},
        ]});
        this.bgl1 = d.createBindGroupLayout({entries:[
            {binding:0,visibility:GPUShaderStage.FRAGMENT,texture:{sampleType:'float'}},
            {binding:1,visibility:GPUShaderStage.FRAGMENT,sampler:{type:'filtering'}},
        ]});
        this.pipeLayout = d.createPipelineLayout({bindGroupLayouts:[this.bgl0,this.bgl1]});
        this.shader = d.createShaderModule({code:vertexShader+'\n'+fragmentShader});
        this.uBG = d.createBindGroup({layout:this.bgl0,entries:[
            {binding:0,resource:{buffer:this.uBuf}},
            {binding:1,resource:{buffer:this.dynBuf,size:32}},
        ]});
    }

    _pipe(sb,db,dm,dw,cm,topo) {
        const k=`${sb}_${db}_${dm}_${dw}_${cm}_${topo}`; let p=this.pipes.get(k); if(p)return p;
        p = this.dev.createRenderPipeline({layout:this.pipeLayout,
            vertex:{module:this.shader,entryPoint:'vs_main',buffers:[VBL]},
            fragment:{module:this.shader,entryPoint:'fs_main',targets:[{format:this.fmt,
                blend:{color:{srcFactor:SBM[sb]||'one',dstFactor:DBM[db]||'zero',operation:'add'},
                       alpha:{srcFactor:SBM[sb]||'one',dstFactor:DBM[db]||'zero',operation:'add'}},writeMask:GPUColorWrite.ALL}]},
            primitive:{topology:'triangle-list',cullMode:'none',frontFace:'cw'},
            depthStencil:{format:'depth32float',depthWriteEnabled:!!dw,depthCompare:DCM[dm]||'always'},
        });
        this.pipes.set(k,p); return p;
    }

    _texBG(t,s) { let b=this.texBGs.get(t); if(b)return b;
        b=this.dev.createBindGroup({layout:this.bgl1,entries:[{binding:0,resource:t.createView()},{binding:1,resource:s}]});
        this.texBGs.set(t,b); return b; }

    uploadVerts(data) {
        const n=data.byteLength;
        if(!this.vBuf||this.vBufSz<n){if(this.vBuf)this.vBuf.destroy();this.vBufSz=Math.max(n,1<<20);
            this.vBuf=this.dev.createBuffer({size:this.vBufSz,usage:GPUBufferUsage.VERTEX|GPUBufferUsage.COPY_DST});}
        this.dev.queue.writeBuffer(this.vBuf,0,data);
    }

    // Convert triangle-strip polys to triangle-list indices
    _buildIndexBuffer(lists) {
        // Count total indices needed
        let totalIdx = 0;
        for (const list of lists) for (const pp of list) {
            if (pp.count < 3 || pp._s < 0) continue;
            totalIdx += (pp.count - 2) * 3;
        }
        const indices = new Uint32Array(totalIdx);
        let idx = 0;
        for (const list of lists) for (const pp of list) {
            if (pp.count < 3 || pp._s < 0) continue;
            pp._idxFirst = idx;
            pp._idxCount = 0;
            for (let i = 0; i < pp.count - 2; i++) {
                const v0 = pp.first + i;
                const v1 = pp.first + i + 1;
                const v2 = pp.first + i + 2;
                if (i & 1) { indices[idx++]=v1; indices[idx++]=v0; indices[idx++]=v2; }
                else       { indices[idx++]=v0; indices[idx++]=v1; indices[idx++]=v2; }
                pp._idxCount += 3;
            }
        }
        // Upload
        const byteLen = idx * 4;
        if (!this.idxBuf || this.idxBufSz < byteLen) {
            if (this.idxBuf) this.idxBuf.destroy();
            this.idxBufSz = Math.max(byteLen, 512*1024);
            this.idxBuf = this.dev.createBuffer({size:this.idxBufSz, usage:GPUBufferUsage.INDEX|GPUBufferUsage.COPY_DST});
        }
        this.dev.queue.writeBuffer(this.idxBuf, 0, indices.buffer, 0, byteLen);
    }

    renderFrame(parsed, texMgr, pvrSnap, vram) {
        const {vertexData,vertexCount,opaque,punchThrough,translucent}=parsed;
        if(!vertexCount)return;
        this.uploadVerts(vertexData);
        this.dev.queue.writeBuffer(this.uBuf,0,this._ndcMat(pvrSnap));
        texMgr.updatePalette(texMgr._lastPvrRegs||new Uint8Array(32768));

        // Stage all frag uniforms
        let slot=0;
        const stage=(list,lt)=>{for(const pp of list){
            if(pp.count<3){pp._s=-1;continue;}
            if(lt==='opaque'&&((pp.isp>>29)&7)===0){pp._s=-1;continue;}
            if(slot>=this.MAX_SLOTS){pp._s=-1;continue;}
            const o=slot*this.SLOT, tsp=pp.tsp, pcw=pp.pcw;
            this.stagingDV.setFloat32(o,1.0,true);
            this.stagingDV.setUint32(o+4,(tsp>>6)&3,true);
            this.stagingDV.setUint32(o+8,(pcw>>3)&1,true);
            this.stagingDV.setUint32(o+12,(tsp>>20)&1,true);
            this.stagingDV.setUint32(o+16,(tsp>>19)&1,true);
            this.stagingDV.setUint32(o+20,(pcw>>2)&1,true);
            this.stagingDV.setUint32(o+24,lt==='punch_through'?1:0,true);
            this.stagingDV.setUint32(o+28,0,true);
            pp._s=slot; slot++;
        }};
        stage(opaque,'opaque'); stage(punchThrough,'punch_through'); stage(translucent,'translucent');
        if(slot>0) this.dev.queue.writeBuffer(this.dynBuf,0,this.staging,0,slot*this.SLOT);

        // Build index buffer: convert strips to triangle lists
        this._buildIndexBuffer([opaque, punchThrough, translucent]);

        const enc=this.dev.createCommandEncoder();
        const rp=enc.beginRenderPass({
            colorAttachments:[{view:this.ctx.getCurrentTexture().createView(),clearValue:{r:0,g:0,b:0,a:1},loadOp:'clear',storeOp:'store'}],
            depthStencilAttachment:{view:this.depth.createView(),depthClearValue:0.0,depthLoadOp:'clear',depthStoreOp:'store'},
        });
        rp.setVertexBuffer(0,this.vBuf);
        if(this.idxBuf) rp.setIndexBuffer(this.idxBuf,'uint32');
        const fb=texMgr.getFallbackTexture(), fbBG=this._texBG(fb.texture,fb.sampler);

        // Flycast forces opaque blend to (ONE, ZERO) — no blending
        for(const pp of opaque){pp.tsp=(pp.tsp&0x03FFFFFF)|(1<<29)|(0<<26);}

        const draw=(list,lt)=>{for(const pp of list){
            if(pp.count<3||pp._s<0)continue;
            const isp=pp.isp,tsp=pp.tsp,tcw=pp.tcw,pcw=pp.pcw;
            let dm=(isp>>29)&7,cm=(isp>>27)&3,zw=(isp>>26)&1?0:1;
            if(lt==='opaque'&&dm===0)continue;
            if(lt==='punch_through'||lt==='translucent')dm=6;
            if(lt==='translucent')zw=0; if(lt==='punch_through')zw=1;
            const pipe=this._pipe((tsp>>29)&7,(tsp>>26)&7,dm,zw,cm^1,'triangle-strip');
            let tbg=fbBG;
            if((pcw>>3)&1){const t=texMgr.getTexture(tsp,tcw,vram);if(t)tbg=this._texBG(t.texture,t.sampler);}
            rp.setPipeline(pipe);
            rp.setBindGroup(0,this.uBG,[pp._s*this.SLOT]);
            rp.setBindGroup(1,tbg);
            rp.drawIndexed(pp._idxCount,1,pp._idxFirst,0,0);
        }};
        draw(opaque,'opaque'); draw(punchThrough,'punch_through'); draw(translucent,'translucent');
        rp.end();
        this.dev.queue.submit([enc.finish()]);
    }

    _ndcMat(snap) {
        const g=snap[0],tx=g&0x3F,ty=(g>>16)&0x3F,w=(tx+1)*32,h=(ty+1)*32;
        const m=new Float32Array(16);
        m[0]=2/w;m[5]=-2/h;m[10]=1;m[12]=-1;m[13]=1;m[15]=1;
        return m;
    }
}
