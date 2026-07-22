import Cocoa
import CoreGraphics
import ApplicationServices

/// A system-wide capture of the numeric **keypad**, so the DAW's monitor volume / transport can be
/// driven while another app is frontmost — you never have to click back into the DAW just to pull
/// the level down. It uses a `CGEventTap` (the only way to *consume* a key so it does not also reach
/// the frontmost app), which requires the **손쉬운 사용(Accessibility)** permission.
///
/// Only a small, dedicated keypad set is captured; every other key passes straight through. The tap
/// exists only while enabled, and only the keypad keys with the numeric-pad flag are consumed, so a
/// laptop's top-row number keys are untouched.
final class GlobalKeypadCapture {
    /// (keyCode, isDown, isRepeat) → true if the key is claimed (swallowed on both down and up so it
    /// never leaks to the frontmost app). `isRepeat` marks an auto-repeat key-down (from holding a
    /// key), so the handler can keep momentary talkback engaged without re-toggling. Return false to
    /// let the key pass through.
    var onKey: ((UInt16, Bool, Bool) -> Bool)?

    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private(set) var isEnabled = false

    /// Every numeric-keypad virtual key code. Which ones actually DO something is decided by `onKey`
    /// (it consults the user's configured monitor-shortcut bindings), so rebinding just works and a
    /// key bound to nothing passes straight through.
    static let capturedKeyCodes: Set<UInt16> = [
        82, 83, 84, 85, 86, 87, 88, 89, 91, 92,   // 0-9
        65,  // .
        67,  // *
        69,  // +
        75,  // /
        76,  // Enter
        78,  // −
        71,  // Clear
        81,  // =
    ]

    /// True when the Accessibility permission is already granted. Pass `prompt: true` to show the
    /// system permission dialog (once) when it is not.
    @discardableResult
    static func accessibilityTrusted(prompt: Bool) -> Bool {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        return AXIsProcessTrustedWithOptions([key: prompt] as CFDictionary)
    }

    /// Turn capture on. Returns false (and does nothing) if Accessibility is not granted — the caller
    /// should tell the user to enable it in 시스템 설정 → 손쉬운 사용. Safe to call repeatedly.
    @discardableResult
    func enable() -> Bool {
        if isEnabled { return true }
        guard Self.accessibilityTrusted(prompt: true) else { return false }

        let mask = (1 << CGEventType.keyDown.rawValue) | (1 << CGEventType.keyUp.rawValue)
        let refcon = Unmanaged.passUnretained(self).toOpaque()
        guard let port = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,               // active tap — may return nil to swallow the event
            eventsOfInterest: CGEventMask(mask),
            callback: { _, type, event, refcon in
                // The system disables a tap if it ever times out; re-arm and pass the event on.
                if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
                    if let refcon {
                        let me = Unmanaged<GlobalKeypadCapture>.fromOpaque(refcon).takeUnretainedValue()
                        if let tap = me.tap { CGEvent.tapEnable(tap: tap, enable: true) }
                    }
                    return Unmanaged.passUnretained(event)
                }
                guard type == .keyDown || type == .keyUp, let refcon else { return Unmanaged.passUnretained(event) }
                let me = Unmanaged<GlobalKeypadCapture>.fromOpaque(refcon).takeUnretainedValue()
                // Only the numeric keypad — the flag distinguishes kp keys from the top row.
                guard event.flags.contains(.maskNumericPad) else { return Unmanaged.passUnretained(event) }
                let keyCode = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
                guard GlobalKeypadCapture.capturedKeyCodes.contains(keyCode) else { return Unmanaged.passUnretained(event) }
                let isDown = (type == .keyDown)
                let isRepeat = isDown && event.getIntegerValueField(.keyboardEventAutorepeat) != 0
                var handled = false
                // The tap runs on the main run loop, so this hop is already on the main thread; the
                // sync dispatch keeps us correct even if the tap is ever moved to another loop.
                if Thread.isMainThread {
                    handled = me.onKey?(keyCode, isDown, isRepeat) ?? false
                } else {
                    DispatchQueue.main.sync { handled = me.onKey?(keyCode, isDown, isRepeat) ?? false }
                }
                return handled ? nil : Unmanaged.passUnretained(event)
            },
            userInfo: refcon
        ) else {
            return false
        }

        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, port, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: port, enable: true)
        tap = port
        runLoopSource = source
        isEnabled = true
        return true
    }

    func disable() {
        guard isEnabled else { return }
        if let tap { CGEvent.tapEnable(tap: tap, enable: false) }
        if let source = runLoopSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes) }
        if let tap { CFMachPortInvalidate(tap) }
        tap = nil
        runLoopSource = nil
        isEnabled = false
    }

    deinit { disable() }
}
