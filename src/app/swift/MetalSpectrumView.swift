import SwiftUI
import MetalKit

/// GPU spectrum renderer. The FFT bins are uploaded to a 1-row texture and sampled with
/// linear filtering in the fragment shader, so the sparse low-frequency bins are
/// interpolated on the GPU instead of drawn as blocky steps. The x axis is log-frequency
/// (20 Hz … 20 kHz), the fill colour is mapped from frequency, and its height is the
/// interpolated magnitude. Falls back (via `isAvailable`) to the Canvas renderer where
/// Metal is missing.
struct MetalSpectrumView: NSViewRepresentable {
    var bins: [Float]
    var sampleRate: Double

    static let isAvailable: Bool = (MTLCreateSystemDefaultDevice() != nil)

    func makeCoordinator() -> Renderer { Renderer() }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: context.coordinator.device)
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.isPaused = true                 // draw only when we push new bins
        view.enableSetNeedsDisplay = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        view.layer?.isOpaque = false
        view.delegate = context.coordinator
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {
        context.coordinator.upload(bins: bins, sampleRate: sampleRate)
        view.needsDisplay = true
    }

    /// Owns the Metal device, pipeline and the bin texture; redraws on each bin update.
    final class Renderer: NSObject, MTKViewDelegate {
        let device: MTLDevice? = MTLCreateSystemDefaultDevice()
        private var queue: MTLCommandQueue?
        private var pipeline: MTLRenderPipelineState?
        private var binTexture: MTLTexture?
        private var textureWidth = 0
        private var params = SpectrumParams(binCount: 0, sampleRate: 48000, fftSize: 2048)
        private var ready = false

        override init() {
            super.init()
            guard let device else { return }
            queue = device.makeCommandQueue()
            buildPipeline(device)
        }

        struct SpectrumParams { var binCount: Float; var sampleRate: Float; var fftSize: Float }

        private func buildPipeline(_ device: MTLDevice) {
            let src = """
            #include <metal_stdlib>
            using namespace metal;
            struct VOut { float4 pos [[position]]; float2 uv; };
            struct Params { float binCount; float sampleRate; float fftSize; };
            vertex VOut v_main(uint vid [[vertex_id]]) {
                float2 p = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
                VOut o;
                o.pos = float4(p, 0.0, 1.0);
                o.uv  = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
                return o;
            }
            static float3 hsv(float h) {
                float3 k = float3(1.0, 2.0/3.0, 1.0/3.0);
                float3 p = abs(fract(float3(h) + k) * 6.0 - 3.0);
                return clamp(p - 1.0, 0.0, 1.0);
            }
            fragment float4 f_main(VOut in [[stage_in]],
                                   texture2d<float> bins [[texture(0)]],
                                   constant Params& P [[buffer(0)]]) {
                constexpr sampler s(filter::linear, address::clamp_to_edge);
                float logMin = log10(20.0), logMax = log10(20000.0);
                float hz = pow(10.0, logMin + in.uv.x * (logMax - logMin));
                float binHz = P.sampleRate / P.fftSize;
                float coord = clamp((hz / binHz) / max(P.binCount, 1.0), 0.0, 1.0);
                float mag = bins.sample(s, float2(coord, 0.5)).r;   // linear-interpolated
                float yBottom = 1.0 - in.uv.y;                      // 0 bottom … 1 top
                if (mag < 0.004 || yBottom > mag) return float4(0.0);
                // frequency-mapped hue: red (low) → violet (high)
                float3 c = hsv(in.uv.x * 0.82);
                float grad = clamp(1.0 - (yBottom / max(mag, 0.001)) * 0.55, 0.35, 1.0);
                return float4(c * (0.55 + 0.45 * mag), grad);
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
                att.rgbBlendOperation = .add
                att.alphaBlendOperation = .add
                att.sourceRGBBlendFactor = .sourceAlpha
                att.sourceAlphaBlendFactor = .sourceAlpha
                att.destinationRGBBlendFactor = .oneMinusSourceAlpha
                att.destinationAlphaBlendFactor = .oneMinusSourceAlpha
                pipeline = try device.makeRenderPipelineState(descriptor: desc)
                ready = true
            } catch {
                ready = false
            }
        }

        func upload(bins: [Float], sampleRate: Double) {
            guard ready, let device, !bins.isEmpty else { return }
            if binTexture == nil || textureWidth != bins.count {
                let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float,
                                                                width: bins.count, height: 1,
                                                                mipmapped: false)
                d.usage = .shaderRead
                binTexture = device.makeTexture(descriptor: d)
                textureWidth = bins.count
            }
            bins.withUnsafeBytes { raw in
                binTexture?.replace(region: MTLRegionMake2D(0, 0, bins.count, 1),
                                    mipmapLevel: 0,
                                    withBytes: raw.baseAddress!,
                                    bytesPerRow: bins.count * MemoryLayout<Float>.stride)
            }
            params = SpectrumParams(binCount: Float(bins.count),
                                    sampleRate: Float(sampleRate > 0 ? sampleRate : 48000),
                                    fftSize: 2048)
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard ready, let queue, let pipeline, let tex = binTexture,
                  let rpd = view.currentRenderPassDescriptor,
                  let drawable = view.currentDrawable,
                  let cmd = queue.makeCommandBuffer(),
                  let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentTexture(tex, index: 0)
            var p = params
            enc.setFragmentBytes(&p, length: MemoryLayout<SpectrumParams>.stride, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
            cmd.present(drawable)
            cmd.commit()
        }
    }
}
