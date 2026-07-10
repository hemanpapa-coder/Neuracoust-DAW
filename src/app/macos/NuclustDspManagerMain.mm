#import <Cocoa/Cocoa.h>

#include "nuclust/NuclustDspManager.h"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>

using neuracoust::nuclust::DspExecutionMode;
using neuracoust::nuclust::LatencyPolicy;
using neuracoust::nuclust::NuclustDspManagerCore;
using neuracoust::nuclust::NuclustDspManagerIpcServer;
using neuracoust::nuclust::NuclustManagerSnapshot;

namespace {

NSString* ns(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()];
}

NSColor* rgb(CGFloat r, CGFloat g, CGFloat b) {
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
}

void drawText(NSString* text, NSRect rect, NSFont* font, NSColor* color, NSTextAlignment alignment = NSTextAlignmentLeft) {
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = alignment;
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    [text drawInRect:rect withAttributes:@{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: color,
        NSParagraphStyleAttributeName: style
    }];
}

void drawPill(NSString* text, NSRect rect, BOOL active, NSColor* color) {
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4.0 yRadius:4.0];
    [[color colorWithAlphaComponent:active ? 0.72 : 0.16] setFill];
    [path fill];
    [[color colorWithAlphaComponent:active ? 0.88 : 0.34] setStroke];
    path.lineWidth = 0.8;
    [path stroke];
    drawText(text, NSInsetRect(rect, 3.0, 2.0), [NSFont systemFontOfSize:8.0 weight:NSFontWeightSemibold], active ? [NSColor whiteColor] : rgb(0.55, 0.58, 0.58), NSTextAlignmentCenter);
}

void drawHorizontalMeter(NSRect rect, double level, double peak) {
    NSBezierPath* bg = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:2.0 yRadius:2.0];
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.30] setFill];
    [bg fill];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.10] setStroke];
    bg.lineWidth = 0.6;
    [bg stroke];
    const CGFloat clamped = static_cast<CGFloat>(std::max(0.0, std::min(1.0, level)));
    const CGFloat width = NSWidth(rect) * clamped;
    auto fillSegment = ^(CGFloat start, CGFloat end, NSColor* color) {
        const CGFloat segmentStart = NSMinX(rect) + NSWidth(rect) * start;
        const CGFloat segmentEnd = NSMinX(rect) + std::min<CGFloat>(width, NSWidth(rect) * end);
        if (segmentEnd <= segmentStart) {
            return;
        }
        [color setFill];
        NSRectFillUsingOperation(NSMakeRect(segmentStart, NSMinY(rect) + 1.0, segmentEnd - segmentStart, NSHeight(rect) - 2.0), NSCompositingOperationSourceOver);
    };
    fillSegment(0.0, 0.72, rgb(0.20, 0.72, 0.42));
    fillSegment(0.72, 0.90, rgb(0.95, 0.72, 0.22));
    fillSegment(0.90, 1.0, rgb(0.95, 0.20, 0.16));
    const CGFloat markerX = NSMinX(rect) + NSWidth(rect) * static_cast<CGFloat>(std::max(0.0, std::min(1.0, peak)));
    NSBezierPath* marker = [NSBezierPath bezierPath];
    marker.lineWidth = 1.2;
    [marker moveToPoint:NSMakePoint(markerX, NSMinY(rect) + 1.0)];
    [marker lineToPoint:NSMakePoint(markerX, NSMaxY(rect) - 1.0)];
    [rgb(0.98, 0.82, 0.32) setStroke];
    [marker stroke];
}

void drawVerticalMeter(NSRect rect, double level) {
    NSBezierPath* bg = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:1.2 yRadius:1.2];
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.32] setFill];
    [bg fill];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.10] setStroke];
    bg.lineWidth = 0.6;
    [bg stroke];
    const CGFloat clamped = static_cast<CGFloat>(std::max(0.0, std::min(1.0, level)));
    const CGFloat height = NSHeight(rect) * clamped;
    if (height > 0.6) {
        NSColor* color = clamped > 0.88 ? rgb(0.95, 0.72, 0.24) : (clamped > 0.68 ? rgb(0.78, 0.70, 0.28) : rgb(0.20, 0.72, 0.42));
        [color setFill];
        NSRectFillUsingOperation(NSMakeRect(NSMinX(rect), NSMaxY(rect) - height, NSWidth(rect), height), NSCompositingOperationSourceOver);
    }
}

} // namespace

@interface NuclustControllerView : NSView
@property(nonatomic, assign) NuclustDspManagerCore* core;
@property(nonatomic, assign) BOOL helpMode;
@end

@implementation NuclustControllerView

- (BOOL)isFlipped { return YES; }

- (NSRect)cardRectAtY:(CGFloat)y width:(CGFloat)cardW {
    return NSMakeRect(18, y, cardW, 94);
}

- (void)showMessage:(NSString*)title text:(NSString*)text {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = text;
    [alert addButtonWithTitle:@"확인"];
    if (self.window != nil) {
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    } else {
        [alert runModal];
    }
}

- (void)mouseDown:(NSEvent*)event {
    if (self.core == nullptr) {
        return;
    }
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NuclustManagerSnapshot snap = self.core->snapshot();
    CGFloat y = 70;
    CGFloat cardW = self.bounds.size.width - 36;
    for (const auto& server : snap.servers) {
        NSRect card = [self cardRectAtY:y width:cardW];
        if (!NSPointInRect(point, card)) {
            y += 108;
            continue;
        }
        const CGFloat left = NSMinX(card) + 10.0;
        const CGFloat right = NSMaxX(card) - 10.0;
        NSRect rst = NSMakeRect(left + 88, y + 76, 35, 13);
        NSRect lat = NSMakeRect(left + 130, y + 76, 31, 13);
        NSRect dsp = NSMakeRect(left + 169, y + 76, 31, 13);
        NSRect idButton = NSMakeRect(right - 70, y + 76, 29, 13);
        NSRect fw = NSMakeRect(right - 36, y + 76, 29, 13);
        if (NSPointInRect(point, rst)) {
            self.core->clearJitterLatch(server.serverId);
        } else if (NSPointInRect(point, lat)) {
            const uint32_t current = snap.settings.defaultBufferFrames;
            const uint32_t next = current == 128 ? 256 : current == 256 ? 512 : current == 512 ? 1024 : 128;
            self.core->setDefaultBufferFrames(next);
        } else if (NSPointInRect(point, dsp)) {
            self.core->setDspEngineRunning(!snap.dspEngineRunning);
        } else if (NSPointInRect(point, idButton)) {
            self.core->identifyServer(server.serverId);
            [self showMessage:@"ID 요청" text:[NSString stringWithFormat:@"%s 서버에 ID 요청을 보냈습니다.", server.name.c_str()]];
        } else if (NSPointInRect(point, fw)) {
            [self showMessage:@"Firmware" text:@"서버 펌웨어/모듈 업데이트 UI는 준비 중입니다. 현재는 상태와 진단 로그에 업데이트 hook만 기록합니다."];
        } else {
            [self showMessage:@"서버 상태"
                         text:[NSString stringWithFormat:@"%s\nIP: %s\nNIC: %s / %s\nCPU: %s\nRAM: %u MB\nRTT: %.2f ms\nJitter: %.0f us current / %.0f us peak\nPackets: %llu in / %llu out / %llu bad\nStatus: %s",
                               server.name.c_str(),
                               server.ipAddress.c_str(),
                               server.interfaceName.c_str(),
                               server.subnet.c_str(),
                               server.cpuModel.c_str(),
                               server.ramMb,
                               server.roundTripMs,
                               server.jitterCurrentMs * 1000.0,
                               server.jitterPeakMs * 1000.0,
                               static_cast<unsigned long long>(server.packetsIn),
                               static_cast<unsigned long long>(server.packetsOut),
                               static_cast<unsigned long long>(server.badPackets),
                               server.message.c_str()]];
        }
        [self setNeedsDisplay:YES];
        return;
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NuclustManagerSnapshot snap = self.core ? self.core->snapshot() : NuclustManagerSnapshot {};
    [rgb(0.055, 0.06, 0.065) setFill];
    NSRectFill(self.bounds);

    drawText(@"Neuracoust DSP Manager", NSMakeRect(18, 12, 300, 24), [NSFont boldSystemFontOfSize:18], rgb(0.92, 0.94, 0.93));
    NSString* status = snap.dspEngineRunning ? @"DSP ENGINE ON" : @"DSP ENGINE STOPPED";
    drawText(status, NSMakeRect(self.bounds.size.width - 180, 14, 160, 20), [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold], snap.dspEngineRunning ? rgb(0.35, 0.9, 0.64) : rgb(0.95, 0.42, 0.36), NSTextAlignmentRight);
    drawText([NSString stringWithFormat:@"IPC 127.0.0.1:48780  |  Buffer %u  |  %@  |  v%s  |  (C) 2026 Neuracoust",
              snap.settings.defaultBufferFrames,
              snap.settings.latencyPolicy == LatencyPolicy::OptimizeStability ? @"Stability" : @"Latency",
              NEURACOUST_DAW_VERSION],
             NSMakeRect(18, 38, self.bounds.size.width - 36, 18),
             [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
             rgb(0.52, 0.56, 0.58));

    CGFloat y = 70;
    CGFloat cardW = self.bounds.size.width - 36;
    if (snap.servers.empty()) {
        [rgb(0.09, 0.1, 0.105) setFill];
        NSBezierPath* empty = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(18, y, cardW, 96) xRadius:6 yRadius:6];
        [empty fill];
        drawText(@"서버 없음", NSMakeRect(34, y + 18, 180, 22), [NSFont boldSystemFontOfSize:16], rgb(0.9, 0.9, 0.88));
        drawText(@"수동 IP를 추가하거나 서버 재검색을 실행해도 앱은 안정적으로 대기합니다.", NSMakeRect(34, y + 46, cardW - 68, 18), [NSFont systemFontOfSize:12], rgb(0.62, 0.66, 0.67));
    }

    for (const auto& server : snap.servers) {
        NSRect card = [self cardRectAtY:y width:cardW];
        [rgb(0.215, 0.222, 0.220) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:card xRadius:5 yRadius:5] fill];
        NSColor* statusColor = server.connected ? rgb(0.18, 0.76, 0.42) : rgb(0.86, 0.52, 0.22);
        [server.jitterPeakLatched ? rgb(0.95, 0.24, 0.20) : [statusColor colorWithAlphaComponent:0.70] setStroke];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(card, 0.5, 0.5) xRadius:6 yRadius:6] stroke];

        const CGFloat left = NSMinX(card) + 10.0;
        const CGFloat right = NSMaxX(card) - 10.0;
        const CGFloat meterLeft = right - 42.0;
        const CGFloat textRight = meterLeft - 12.0;
        drawText(ns(server.name), NSMakeRect(left, y + 8, textRight - left, 14), [NSFont systemFontOfSize:10.5 weight:NSFontWeightSemibold], rgb(0.92, 0.94, 0.93));
        NSString* ready = server.connected ? [NSString stringWithFormat:@"READY · RTT %.2f ms", server.roundTripMs] : @"CHECKING";
        drawText(ready, NSMakeRect(left, y + 24, 120, 11), [NSFont systemFontOfSize:8.2 weight:NSFontWeightBold], statusColor);
        drawText(ns(server.ipAddress), NSMakeRect(left, y + 38, 72, 10), [NSFont monospacedSystemFontOfSize:7.8 weight:NSFontWeightMedium], rgb(0.58, 0.62, 0.62));

        std::ostringstream deviceLine;
        deviceLine << server.maxChannels << " ch · " << std::fixed << std::setprecision(1) << server.temperatureC << " C · " << server.interfaceName << " · " << server.nicLinkSpeedMbps << "MbE";
        drawText(ns(deviceLine.str()), NSMakeRect(left + 78, y + 38, textRight - (left + 78), 10), [NSFont monospacedSystemFontOfSize:7.8 weight:NSFontWeightMedium], rgb(0.58, 0.62, 0.62));

        drawText(@"RX", NSMakeRect(left, y + 54, 16, 8), [NSFont monospacedSystemFontOfSize:6.8 weight:NSFontWeightMedium], rgb(0.58, 0.62, 0.62));
        drawHorizontalMeter(NSMakeRect(left + 18, y + 53, 132, 8), server.inputLevel, server.peakHoldLevel);
        drawText(@"TX", NSMakeRect(left, y + 66, 16, 8), [NSFont monospacedSystemFontOfSize:6.8 weight:NSFontWeightMedium], rgb(0.58, 0.62, 0.62));
        drawHorizontalMeter(NSMakeRect(left + 18, y + 65, 132, 8), server.outputLevel, server.peakHoldLevel);

        NSInteger core = 0;
        for (double usage : server.coreUsage) {
            if (core >= 12) break;
            drawVerticalMeter(NSMakeRect(meterLeft + core * 5.6, y + 23, 3.5, 42), usage);
            ++core;
        }
        drawText(@"CPU", NSMakeRect(meterLeft - 2, y + 12, right - meterLeft, 9), [NSFont monospacedSystemFontOfSize:6.8 weight:NSFontWeightMedium], rgb(0.58, 0.62, 0.62), NSTextAlignmentCenter);

        std::ostringstream metrics;
        metrics << "Core M1 D1 P" << std::max(1, static_cast<int>(server.coreUsage.size()) - 2)
                << "   CH " << server.usedChannels << "/" << server.maxChannels
                << "   BUF " << server.networkBufferFrames << "/" << std::fixed << std::setprecision(1)
                << (static_cast<double>(server.networkBufferFrames) * 1000.0 / 48000.0)
                << "   Lat " << server.reportedLatencySamples << " smp";
        drawText(ns(metrics.str()), NSMakeRect(left + 168, y + 54, textRight - (left + 168), 10), [NSFont monospacedSystemFontOfSize:7.4 weight:NSFontWeightSemibold], rgb(0.70, 0.73, 0.72));

        std::ostringstream moduleText;
        moduleText << "Modules ";
        for (const auto& module : server.modules) {
            moduleText << (module.installed ? "[OK] " : "[--] ") << module.moduleId << " ";
        }
        drawText(ns(moduleText.str()), NSMakeRect(left + 168, y + 68, textRight - (left + 168), 10), [NSFont monospacedSystemFontOfSize:7.2 weight:NSFontWeightRegular], rgb(0.58, 0.62, 0.62));

        const BOOL jitterActive = server.jitterPeakLatched;
        drawText([NSString stringWithFormat:@"Jit %.0f/%.0f us", server.jitterCurrentMs * 1000.0, server.jitterPeakMs * 1000.0], NSMakeRect(left, y + 79, 82, 9), [NSFont monospacedSystemFontOfSize:7.2 weight:NSFontWeightSemibold], jitterActive ? rgb(1.0, 0.24, 0.20) : rgb(0.58, 0.62, 0.62));
        drawPill(@"RST", NSMakeRect(left + 88, y + 76, 35, 13), jitterActive, rgb(0.86, 0.38, 0.30));
        drawPill(@"LAT", NSMakeRect(left + 130, y + 76, 31, 13), YES, rgb(0.18, 0.62, 0.86));
        drawPill(@"DSP", NSMakeRect(left + 169, y + 76, 31, 13), snap.dspEngineRunning, rgb(0.12, 0.44, 0.92));
        drawPill(@"ID", NSMakeRect(right - 70, y + 76, 29, 13), server.connected, rgb(0.12, 0.44, 0.92));
        drawPill(@"FW", NSMakeRect(right - 36, y + 76, 29, 13), NO, rgb(0.88, 0.66, 0.24));

        if (self.helpMode) {
            drawText(@"DAW Remote DSP card: RTT, buffer/latency, jitter latch, In/Out levels, CPU cores, modules, ID/FW controls.", NSMakeRect(left + 190, y + 78, textRight - (left + 190), 10), [NSFont systemFontOfSize:8], rgb(0.88, 0.78, 0.42));
        }

        y += 108;
    }

    drawText([NSString stringWithFormat:@"Plugins registered: %lu", static_cast<unsigned long>(snap.pluginInstances.size())],
             NSMakeRect(18, self.bounds.size.height - 28, 180, 16),
             [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
             rgb(0.52, 0.56, 0.58));
}

@end

@interface NuclustAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NuclustControllerView* controllerView;
@property(nonatomic, strong) NSButton* logCopyButton;
@property(nonatomic, strong) NSTimer* timer;
@end

@implementation NuclustAppDelegate {
    std::unique_ptr<NuclustDspManagerCore> _core;
    std::unique_ptr<NuclustDspManagerIpcServer> _ipc;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    _core = std::make_unique<NuclustDspManagerCore>();
    _ipc = std::make_unique<NuclustDspManagerIpcServer>(*_core);
    _ipc->start(48780);
    [self installStatusMenu];
    [self openManager:nil];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(refresh:) userInfo:nil repeats:YES];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (_ipc) _ipc->stop();
    if (_core) _core->saveSettings();
}

- (void)installStatusMenu {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    self.statusItem.button.image = [NSImage imageWithSystemSymbolName:@"cpu" accessibilityDescription:@"Neuracoust DSP Manager"];
    self.statusItem.button.title = @" Neuracoust";
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Neuracoust DSP Manager"];
    [menu addItemWithTitle:@"Neuracoust DSP Manager 열기" action:@selector(openManager:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"서버 재검색" action:@selector(rescan:) keyEquivalent:@""].target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"DSP 엔진 시작" action:@selector(startEngine:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"DSP 엔진 중지" action:@selector(stopEngine:) keyEquivalent:@""].target = self;
    NSMenuItem* bufferItem = [[NSMenuItem alloc] initWithTitle:@"버퍼 프리셋" action:nil keyEquivalent:@""];
    NSMenu* bufferMenu = [[NSMenu alloc] initWithTitle:@"버퍼 프리셋"];
    for (NSNumber* frames in @[@128, @256, @512, @1024]) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:[frames stringValue] action:@selector(bufferPreset:) keyEquivalent:@""];
        item.target = self;
        item.representedObject = frames;
        [bufferMenu addItem:item];
    }
    bufferItem.submenu = bufferMenu;
    [menu addItem:bufferItem];
    [menu addItemWithTitle:@"로그/진단 열기" action:@selector(openLogs:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"로그 복사" action:@selector(copyLogs:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"도움말 모드" action:@selector(toggleHelp:) keyEquivalent:@""].target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"종료" action:@selector(terminate:) keyEquivalent:@""].target = NSApp;
    self.statusItem.menu = menu;
}

- (void)openManager:(id)sender {
    (void)sender;
    if (!self.window) {
        self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, 920, 640)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        self.window.title = @"Neuracoust DSP Manager";
        self.controllerView = [[NuclustControllerView alloc] initWithFrame:self.window.contentView.bounds];
        self.controllerView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.controllerView.core = _core.get();
        self.window.contentView = self.controllerView;
        self.logCopyButton = [NSButton buttonWithTitle:@"Copy Log" target:self action:@selector(copyLogs:)];
        self.logCopyButton.bezelStyle = NSBezelStyleRounded;
        self.logCopyButton.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
        self.logCopyButton.frame = NSMakeRect(self.window.contentView.bounds.size.width - 280, 12, 86, 24);
        self.logCopyButton.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        [self.window.contentView addSubview:self.logCopyButton];
    }
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)refresh:(id)sender {
    (void)sender;
    if (_core) {
        _core->refreshServerTelemetry();
    }
    [self.controllerView setNeedsDisplay:YES];
}

- (void)rescan:(id)sender {
    (void)sender;
    _core->rescanServers();
    [self.controllerView setNeedsDisplay:YES];
}

- (void)startEngine:(id)sender { (void)sender; _core->setDspEngineRunning(true); }
- (void)stopEngine:(id)sender { (void)sender; _core->setDspEngineRunning(false); }
- (void)bufferPreset:(NSMenuItem*)sender { _core->setDefaultBufferFrames([sender.representedObject unsignedIntValue]); }
- (void)toggleHelp:(id)sender { (void)sender; self.controllerView.helpMode = !self.controllerView.helpMode; [self.controllerView setNeedsDisplay:YES]; }

- (void)openLogs:(id)sender {
    (void)sender;
    NSString* path = ns(_core->logPath().string());
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)copyLogs:(id)sender {
    (void)sender;
    NSString* diagnostics = ns(_core->diagnosticText());
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:diagnostics forType:NSPasteboardTypeString];
    self.logCopyButton.title = @"Copied";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        self.logCopyButton.title = @"Copy Log";
    });
}

@end

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyAccessory;
        NuclustAppDelegate* delegate = [[NuclustAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
