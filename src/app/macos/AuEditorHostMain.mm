#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUCocoaUIView.h>

#include "audio/WavFile.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

namespace {

struct FixturePlayback {
    std::vector<float> stereo;
    size_t positionFrames = 0;
};

OSStatus fixtureInputCallback(void* refCon,
                              AudioUnitRenderActionFlags*,
                              const AudioTimeStamp*,
                              UInt32,
                              UInt32 frameCount,
                              AudioBufferList* ioData) {
    auto* fixture = static_cast<FixturePlayback*>(refCon);
    if (fixture == nullptr || ioData == nullptr) return kAudio_ParamError;
    for (UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex) {
        auto* output = static_cast<float*>(ioData->mBuffers[bufferIndex].mData);
        if (output == nullptr) continue;
        const bool interleaved = ioData->mNumberBuffers == 1 && ioData->mBuffers[bufferIndex].mNumberChannels >= 2;
        for (UInt32 frame = 0; frame < frameCount; ++frame) {
            const size_t sourceFrame = fixture->positionFrames + frame;
            const float left = sourceFrame * 2u < fixture->stereo.size() ? fixture->stereo[sourceFrame * 2u] : 0.0f;
            const float right = sourceFrame * 2u + 1u < fixture->stereo.size() ? fixture->stereo[sourceFrame * 2u + 1u] : 0.0f;
            if (interleaved) { output[frame * 2u] = left; output[frame * 2u + 1u] = right; }
            else output[frame] = bufferIndex == 0 ? left : right;
        }
        ioData->mBuffers[bufferIndex].mDataByteSize = frameCount * sizeof(float) * (interleaved ? 2u : 1u);
    }
    fixture->positionFrames += frameCount;
    return noErr;
}

NSString* const kPluginEditorTransportToggleNotification = @"com.neuracoust.daw.pluginEditor.transport.toggle";

std::string argumentValue(int argc, const char* argv[], const char* key) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], key) == 0) {
            return argv[index + 1] != nullptr ? argv[index + 1] : "";
        }
    }
    return {};
}

bool hasArgument(int argc, const char* argv[], const char* key) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], key) == 0) {
            return true;
        }
    }
    return false;
}

void logStage(const char* stage) {
    char cwd[PATH_MAX] {};
    getcwd(cwd, sizeof(cwd));
    std::cout << "AU_STAGE " << stage << " cwd=" << cwd << std::endl;
}

OSType fourCharCode(NSString* text) {
    if (text.length < 4) {
        return 0;
    }
    const char* value = text.UTF8String;
    if (value == nullptr || std::strlen(value) < 4) {
        return 0;
    }
    return (static_cast<OSType>(static_cast<unsigned char>(value[0])) << 24) |
           (static_cast<OSType>(static_cast<unsigned char>(value[1])) << 16) |
           (static_cast<OSType>(static_cast<unsigned char>(value[2])) << 8) |
           static_cast<OSType>(static_cast<unsigned char>(value[3]));
}

NSString* normalizedToken(NSString* text) {
    NSMutableString* out = [NSMutableString string];
    NSString* lower = text.lowercaseString ?: @"";
    for (NSUInteger index = 0; index < lower.length; ++index) {
        unichar ch = [lower characterAtIndex:index];
        if ([[NSCharacterSet alphanumericCharacterSet] characterIsMember:ch]) {
            [out appendFormat:@"%C", ch];
        }
    }
    return out;
}

NSString* componentPathForPluginName(NSString* pluginName) {
    NSArray<NSString*>* roots = @[@"/Library/Audio/Plug-Ins/Components",
                                  [@"~/Library/Audio/Plug-Ins/Components" stringByExpandingTildeInPath]];
    NSString* wanted = normalizedToken(pluginName ?: @"");
    NSFileManager* manager = [NSFileManager defaultManager];
    for (NSString* root in roots) {
        NSArray<NSString*>* entries = [manager contentsOfDirectoryAtPath:root error:nil];
        for (NSString* entry in entries) {
            if (![entry.pathExtension.lowercaseString isEqualToString:@"component"]) {
                continue;
            }
            NSString* full = [root stringByAppendingPathComponent:entry];
            NSDictionary* plist = [NSDictionary dictionaryWithContentsOfFile:[full stringByAppendingPathComponent:@"Contents/Info.plist"]];
            NSArray* components = plist[@"AudioComponents"];
            NSString* display = components.count > 0 ? components[0][@"description"] : plist[@"CFBundleName"];
            NSString* name = components.count > 0 ? components[0][@"name"] : @"";
            NSString* combined = normalizedToken([NSString stringWithFormat:@"%@ %@ %@", display ?: @"", name ?: @"", entry.stringByDeletingPathExtension]);
            if (wanted.length > 0 && [combined containsString:wanted]) {
                return full;
            }
        }
    }
    return nil;
}

BOOL readAudioComponentDescription(NSString* componentPath, AudioComponentDescription* outDescription, NSString** outName) {
    if (componentPath.length == 0 || outDescription == nullptr) {
        return NO;
    }
    NSDictionary* plist = [NSDictionary dictionaryWithContentsOfFile:[componentPath stringByAppendingPathComponent:@"Contents/Info.plist"]];
    NSArray* components = plist[@"AudioComponents"];
    NSDictionary* component = components.count > 0 ? components[0] : nil;
    if (![component isKindOfClass:[NSDictionary class]]) {
        return NO;
    }
    memset(outDescription, 0, sizeof(*outDescription));
    outDescription->componentType = fourCharCode(component[@"type"]);
    outDescription->componentSubType = fourCharCode(component[@"subtype"]);
    outDescription->componentManufacturer = fourCharCode(component[@"manufacturer"]);
    outDescription->componentFlags = 0;
    outDescription->componentFlagsMask = 0;
    if (outName != nullptr) {
        *outName = component[@"description"] ?: component[@"name"] ?: plist[@"CFBundleName"];
    }
    return outDescription->componentType != 0 &&
           outDescription->componentSubType != 0 &&
           outDescription->componentManufacturer != 0;
}

bool responderAcceptsTextInput(NSResponder* responder) {
    if (responder == nil) {
        return false;
    }
    if ([responder isKindOfClass:[NSTextView class]] || [responder isKindOfClass:[NSTextField class]]) {
        return true;
    }
    if ([responder isKindOfClass:[NSComboBox class]] || [responder isKindOfClass:[NSSearchField class]]) {
        return true;
    }
    return false;
}

bool isPlainSpaceKeyEvent(NSEvent* event) {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & (NSEventModifierFlagCommand |
                               NSEventModifierFlagOption |
                               NSEventModifierFlagShift |
                               NSEventModifierFlagControl);
    if (modifiers != 0) {
        return false;
    }
    NSString* chars = event.charactersIgnoringModifiers ?: @"";
    return chars.length == 1 && [chars characterAtIndex:0] == ' ';
}

void postPluginEditorTransportToggle() {
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:kPluginEditorTransportToggleNotification
                                                                   object:nil
                                                                 userInfo:nil
                                                       deliverImmediately:YES];
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const std::string componentArgument = argumentValue(argc, argv, "--component");
        const std::string nameArgument = argumentValue(argc, argv, "--name");
        const std::string titleArgument = argumentValue(argc, argv, "--title");
        const bool probeMode = hasArgument(argc, argv, "--probe");
        const std::string inputArgument = argumentValue(argc, argv, "--input");
        const std::string holdArgument = argumentValue(argc, argv, "--hold-seconds");
        const int holdSeconds = holdArgument.empty() ? 20 : std::max(0, std::atoi(holdArgument.c_str()));

        logStage("app.begin");
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        logStage("app.ready");

        NSString* componentPath = componentArgument.empty()
            ? componentPathForPluginName([NSString stringWithUTF8String:nameArgument.c_str()])
            : [NSString stringWithUTF8String:componentArgument.c_str()];
        if (componentPath.length == 0) {
            std::cerr << "AU_ERROR No matching Audio Unit component was found." << std::endl;
            return 2;
        }

        AudioComponentDescription desc {};
        NSString* componentName = nil;
        if (!readAudioComponentDescription(componentPath, &desc, &componentName)) {
            std::cerr << "AU_ERROR Audio Unit component metadata is invalid." << std::endl;
            return 3;
        }
        logStage("component.metadata.ok");

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (component == nullptr) {
            std::cerr << "AU_ERROR Audio Unit component is not registered." << std::endl;
            return 4;
        }
        logStage("component.find.ok");

        AudioUnit unit = nullptr;
        OSStatus status = AudioComponentInstanceNew(component, &unit);
        if (status != noErr || unit == nullptr) {
            std::cerr << "AU_ERROR AudioComponentInstanceNew failed: " << status << std::endl;
            return 5;
        }
        logStage("unit.new.ok");

        auto* fixture = new FixturePlayback();
        double fixtureSampleRate = 48000.0;
        int64_t fixtureFrames = 0;
        if (!inputArgument.empty()) {
            neuracoust::daw::WavAudioData audio;
            std::string readError;
            if (!neuracoust::daw::readPcmWavFile(inputArgument, audio, readError) || audio.channels < 1) {
                std::cerr << "AU_ERROR Could not read fixture: " << readError << std::endl;
                delete fixture; AudioComponentInstanceDispose(unit); return 12;
            }
            fixtureSampleRate = audio.sampleRate;
            fixtureFrames = audio.frameCount();
            fixture->stereo.resize(static_cast<size_t>(fixtureFrames) * 2u);
            for (int64_t frameIndex = 0; frameIndex < fixtureFrames; ++frameIndex) {
                fixture->stereo[static_cast<size_t>(frameIndex) * 2u] = audio.interleavedSamples[static_cast<size_t>(frameIndex) * audio.channels];
                fixture->stereo[static_cast<size_t>(frameIndex) * 2u + 1u] = audio.interleavedSamples[static_cast<size_t>(frameIndex) * audio.channels + std::min(1, audio.channels - 1)];
            }
            AudioStreamBasicDescription format {};
            format.mSampleRate = fixtureSampleRate; format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
            format.mBytesPerPacket = sizeof(float); format.mFramesPerPacket = 1;
            format.mBytesPerFrame = sizeof(float); format.mChannelsPerFrame = 2; format.mBitsPerChannel = 32;
            UInt32 maxFrames = 256;
            AURenderCallbackStruct callback { fixtureInputCallback, fixture };
            AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));
            AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
            AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format));
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        }

        status = AudioUnitInitialize(unit);
        if (status != noErr) {
            std::cerr << "AU_ERROR AudioUnitInitialize failed: " << status << std::endl;
            AudioComponentInstanceDispose(unit);
            return 6;
        }
        logStage("unit.initialize.ok");

        UInt32 propertySize = 0;
        Boolean writable = false;
        status = AudioUnitGetPropertyInfo(unit,
                                          kAudioUnitProperty_CocoaUI,
                                          kAudioUnitScope_Global,
                                          0,
                                          &propertySize,
                                          &writable);
        if (status != noErr || propertySize < sizeof(AudioUnitCocoaViewInfo)) {
            std::cerr << "AU_ERROR Audio Unit did not publish a Cocoa UI: " << status << std::endl;
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            return 7;
        }
        auto* viewInfo = static_cast<AudioUnitCocoaViewInfo*>(std::calloc(1, propertySize));
        status = AudioUnitGetProperty(unit,
                                      kAudioUnitProperty_CocoaUI,
                                      kAudioUnitScope_Global,
                                      0,
                                      viewInfo,
                                      &propertySize);
        if (status != noErr || viewInfo == nullptr || viewInfo->mCocoaAUViewBundleLocation == nullptr || viewInfo->mCocoaAUViewClass[0] == nullptr) {
            std::cerr << "AU_ERROR Could not read Audio Unit Cocoa UI: " << status << std::endl;
            if (viewInfo != nullptr) {
                std::free(viewInfo);
            }
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            return 8;
        }
        logStage("cocoa.info.ok");

        NSURL* bundleURL = (NSURL*)viewInfo->mCocoaAUViewBundleLocation;
        NSString* className = (NSString*)viewInfo->mCocoaAUViewClass[0];
        NSBundle* viewBundle = [NSBundle bundleWithURL:bundleURL];
        if (viewBundle == nil || ![viewBundle load]) {
            std::cerr << "AU_ERROR Could not load Audio Unit view bundle." << std::endl;
            std::free(viewInfo);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            return 9;
        }
        Class factoryClass = [viewBundle classNamed:className];
        id factory = factoryClass != Nil ? [[factoryClass alloc] init] : nil;
        if (factory == nil || ![factory conformsToProtocol:@protocol(AUCocoaUIBase)]) {
            std::cerr << "AU_ERROR Audio Unit view factory class is unavailable." << std::endl;
            [factory release];
            std::free(viewInfo);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            return 10;
        }
        logStage("cocoa.factory.ok");

        NSView* view = [(id<AUCocoaUIBase>)factory uiViewForAudioUnit:unit withSize:NSMakeSize(900, 620)];
        if (view == nil) {
            std::cerr << "AU_ERROR Audio Unit Cocoa view factory returned nil." << std::endl;
            [factory release];
            std::free(viewInfo);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            return 11;
        }
        [view retain];
        logStage("view.create.ok");

        NSRect frame = view.frame;
        if (NSWidth(frame) < 120 || NSHeight(frame) < 120) {
            frame = NSMakeRect(0, 0, 900, 620);
            view.frame = frame;
        }
        NSString* panelTitle = titleArgument.empty()
            ? (componentName ?: @"Audio Unit")
            : [NSString stringWithUTF8String:titleArgument.c_str()];
        NSPanel* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, NSWidth(frame), NSHeight(frame))
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        panel.title = panelTitle;
        panel.releasedWhenClosed = NO;
        panel.hidesOnDeactivate = NO;
        panel.level = NSFloatingWindowLevel;
        panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorMoveToActiveSpace;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        panel.contentView = view;
        [panel center];
        [panel makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        logStage("view.attach.ok");
        std::cout << "READY" << std::endl;

        if (!inputArgument.empty()) {
            AudioUnit renderUnit = unit;
            const auto totalFrames = fixtureFrames;
            const auto sampleRate = fixtureSampleRate;
            std::thread([fixture, renderUnit, totalFrames, sampleRate, holdSeconds]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                constexpr UInt32 blockSize = 256;
                std::vector<float> left(blockSize), right(blockSize);
                const auto start = std::chrono::steady_clock::now();
                for (int64_t position = 0; position < totalFrames; position += blockSize) {
                    const UInt32 frames = static_cast<UInt32>(std::min<int64_t>(blockSize, totalFrames - position));
                    struct { UInt32 count; AudioBuffer buffers[2]; } storage {};
                    storage.count = 2;
                    storage.buffers[0] = { 1, frames * static_cast<UInt32>(sizeof(float)), left.data() };
                    storage.buffers[1] = { 1, frames * static_cast<UInt32>(sizeof(float)), right.data() };
                    AudioTimeStamp timestamp {}; timestamp.mFlags = kAudioTimeStampSampleTimeValid; timestamp.mSampleTime = position;
                    AudioUnitRenderActionFlags flags = 0;
                    const OSStatus renderStatus = AudioUnitRender(renderUnit, &flags, &timestamp, 0, frames,
                                                                 reinterpret_cast<AudioBufferList*>(&storage));
                    if (renderStatus != noErr) { std::cerr << "AU_RENDER_ERROR " << renderStatus << std::endl; break; }
                    std::this_thread::sleep_until(start + std::chrono::duration<double>(static_cast<double>(position + frames) / sampleRate));
                }
                std::cout << "AU_PLAYBACK_COMPLETE elapsed=" << (static_cast<double>(totalFrames) / sampleRate) << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(holdSeconds));
                delete fixture;
                dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
            }).detach();
        }

        if (probeMode) {
            _Exit(0);
        }
        __block AudioUnit blockUnit = unit;
        __block id blockFactory = factory;
        __block AudioUnitCocoaViewInfo* blockViewInfo = viewInfo;
        __block id transportKeyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                                               handler:^NSEvent* (NSEvent* event) {
            if (event.window != panel || !isPlainSpaceKeyEvent(event)) {
                return event;
            }
            if (responderAcceptsTextInput(panel.firstResponder)) {
                return event;
            }
            postPluginEditorTransportToggle();
            return nil;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                          object:panel
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification*) {
            if (transportKeyMonitor != nil) {
                [NSEvent removeMonitor:transportKeyMonitor];
                transportKeyMonitor = nil;
            }
            [view release];
            [blockFactory release];
            if (blockViewInfo != nullptr) {
                std::free(blockViewInfo);
            }
            AudioUnitUninitialize(blockUnit);
            AudioComponentInstanceDispose(blockUnit);
            [NSApp terminate:nil];
        }];
        [NSApp run];
        _Exit(0);
    }
}
