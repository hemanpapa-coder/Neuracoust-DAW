import SwiftUI
import MetalKit

/// GPU-rendered bar-line grid for the conductor bar's lane column. Bar lines are computed
/// entirely in the fragment shader from the visible span (in bars), so there is no
/// per-line geometry — a pixel lights up when it sits on a bar boundary. Downbeats every
/// `barsPerGroup` bars are drawn brighter. Falls back to nothing where Metal is missing;
/// the caller keeps a Canvas grid for that case.
struct MetalGridView: NSViewRepresentable {
    var startBars: Double      // visibleStart / secondsPerBar
    var spanBars: Double       // visibleDuration / secondsPerBar
    var barsPerGroup: Int      // beats-per-bar analogue for accenting downbeats

    static let isAvailable: Bool = (MTLCreateSystemDefaultDevice() != nil)

    func makeCoordinator() -> Renderer { Renderer() }

    func makeNSView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: context.coordinator.device)
        v.colorPixelFormat = .bgra8Unorm
        v.framebufferOnly = true
        v.isPaused = true
        v.enableSetNeedsDisplay = true
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        v.layer?.isOpaque = false
        v.delegate = context.coordinator
        return v
    }

    func updateNSView(_ v: MTKView, context: Context) {
        context.coordinator.params = Renderer.GridParams(
            startBars: Float(startBars), spanBars: Float(max(0.0001, spanBars)),
            pxWidth: Float(max(1, v.drawableSize.width)), barsPerGroup: Float(max(1, barsPerGroup)))
        v.needsDisplay = true
    }

    final class Renderer: NSObject, MTKViewDelegate {
        let device: MTLDevice? = MTLCreateSystemDefaultDevice()
        private var queue: MTLCommandQueue?
        private var pipeline: MTLRenderPipelineState?
        private var ready = false
        var params = GridParams(startBars: 0, spanBars: 16, pxWidth: 800, barsPerGroup: 4)

        struct GridParams { var startBars: Float; var spanBars: Float; var pxWidth: Float; var barsPerGroup: Float }

        override init() {
            super.init()
            guard let device else { return }
            queue = device.makeCommandQueue()
            let src = """
            #include <metal_stdlib>
            using namespace metal;
            struct VOut { float4 pos [[position]]; float2 uv; };
            struct P { float startBars; float spanBars; float pxWidth; float barsPerGroup; };
            vertex VOut v_main(uint vid [[vertex_id]]) {
                float2 p = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
                VOut o; o.pos = float4(p, 0.0, 1.0);
                o.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
                return o;
            }
            fragment float4 f_main(VOut in [[stage_in]], constant P& u [[buffer(0)]]) {
                float barPos = u.startBars + in.uv.x * u.spanBars;
                float barsPerPixel = u.spanBars / max(u.pxWidth, 1.0);
                float f = fract(barPos);
                float d = min(f, 1.0 - f);                       // distance to nearest bar line
                float line = 1.0 - smoothstep(0.0, barsPerPixel * 1.1, d);
                // brighter downbeat every barsPerGroup bars
                float grp = fract(barPos / u.barsPerGroup) * u.barsPerGroup;
                float dg = min(grp, u.barsPerGroup - grp);
                float down = 1.0 - smoothstep(0.0, barsPerPixel * 1.1, dg);
                float a = line * 0.09 + down * 0.10;
                return float4(1.0, 1.0, 1.0, clamp(a, 0.0, 0.22));
            }
            """
            do {
                let lib = try device.makeLibrary(source: src, options: nil)
                let desc = MTLRenderPipelineDescriptor()
                desc.vertexFunction = lib.makeFunction(name: "v_main")
                desc.fragmentFunction = lib.makeFunction(name: "f_main")
                let att = desc.colorAttachments[0]!
                att.pixelFormat = .bgra8Unorm
                att.isBlendingEnabled = true
                att.rgbBlendOperation = .add; att.alphaBlendOperation = .add
                att.sourceRGBBlendFactor = .sourceAlpha
                att.sourceAlphaBlendFactor = .sourceAlpha
                att.destinationRGBBlendFactor = .oneMinusSourceAlpha
                att.destinationAlphaBlendFactor = .oneMinusSourceAlpha
                pipeline = try device.makeRenderPipelineState(descriptor: desc)
                ready = true
            } catch { ready = false }
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard ready, let queue, let pipeline,
                  let rpd = view.currentRenderPassDescriptor,
                  let drawable = view.currentDrawable,
                  let cmd = queue.makeCommandBuffer(),
                  let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
            enc.setRenderPipelineState(pipeline)
            var p = params
            enc.setFragmentBytes(&p, length: MemoryLayout<GridParams>.stride, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
            cmd.present(drawable)
            cmd.commit()
        }
    }
}
