import CoreAudio
import Foundation

// 1) Create a stereo global process tap (tap everything; unmuted so apps still play).
let desc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
desc.isPrivate = true
desc.muteBehavior = .unmuted
desc.name = "NC Reference Tap Test"

var tapID = AudioObjectID(kAudioObjectUnknown)
let cs = AudioHardwareCreateProcessTap(desc, &tapID)
print("CreateProcessTap status=\(cs) tapID=\(tapID)")
guard cs == noErr, tapID != kAudioObjectUnknown else { print("TAP FAILED (permission? status=\(cs))"); exit(1) }

let tapUID = desc.uuid.uuidString
print("tap UID=\(tapUID)")

// 2) Aggregate device that reads the tap.
let aggUID = "nc-tap-agg-\(tapUID)"
let aggDict: [String: Any] = [
    kAudioAggregateDeviceNameKey as String: "NC Tap Agg",
    kAudioAggregateDeviceUIDKey as String: aggUID,
    kAudioAggregateDeviceIsPrivateKey as String: 1,
    kAudioAggregateDeviceTapAutoStartKey as String: 1,
    kAudioAggregateDeviceTapListKey as String: [[ kAudioSubTapUIDKey as String: tapUID ]],
]
var aggID = AudioObjectID(kAudioObjectUnknown)
let ac = AudioHardwareCreateAggregateDevice(aggDict as CFDictionary, &aggID)
print("CreateAggregate status=\(ac) aggID=\(aggID)")
guard ac == noErr, aggID != kAudioObjectUnknown else { print("AGG FAILED status=\(ac)"); exit(1) }

// 3) IOProc reading the tap audio; track peak.
var peak: Float = 0
final class Box { var peak: Float = 0; var blocks = 0 }
let box = Box()
let boxPtr = Unmanaged.passRetained(box).toOpaque()
// Input stream format on the aggregate (confirms it exposes the tapped audio as input).
var fmt = AudioStreamBasicDescription()
var fsz = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
var fa = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyVirtualFormat, mScope: kAudioObjectPropertyScopeInput, mElement: 0)
let fst = AudioObjectGetPropertyData(aggID, &fa, 0, nil, &fsz, &fmt)
print("input virtual format status=\(fst) ch=\(fmt.mChannelsPerFrame) sr=\(fmt.mSampleRate)")

let ioq = DispatchQueue(label: "nc.tap.io")
var procID: AudioDeviceIOProcID?
let ioStatus = AudioDeviceCreateIOProcIDWithBlock(&procID, aggID, ioq) { (_, inInput, _, _, _) in
    let abl = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inInput))
    for buf in abl {
        guard let data = buf.mData else { continue }
        let n = Int(buf.mDataByteSize) / MemoryLayout<Float>.size
        let p = data.assumingMemoryBound(to: Float.self)
        for i in 0..<n { box.peak = max(box.peak, abs(p[i])) }
    }
    box.blocks += 1
}
_ = boxPtr
print("CreateIOProc status=\(ioStatus)")
guard ioStatus == noErr, let procID else { print("IOPROC FAILED status=\(ioStatus)"); exit(1) }

// aggregate nominal sample rate
var sr: Float64 = 0; var srsz = UInt32(8)
var sra = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
AudioObjectGetPropertyData(aggID, &sra, 0, nil, &srsz, &sr)
print("aggregate nominal SR=\(sr)")
let startStatus = AudioDeviceStart(aggID, procID)
print("AudioDeviceStart status=\(startStatus)")
Thread.sleep(forTimeInterval: 3.0)
AudioDeviceStop(aggID, procID)
AudioDeviceDestroyIOProcID(aggID, procID)
print("RESULT: blocks=\(box.blocks) peak=\(box.peak)   (peak>0 = captured other apps' audio)")

// 4) Cleanup.
AudioHardwareDestroyAggregateDevice(aggID)
AudioHardwareDestroyProcessTap(tapID)
print("cleaned up OK")
