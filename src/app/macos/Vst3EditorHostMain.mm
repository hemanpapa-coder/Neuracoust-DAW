#import <Cocoa/Cocoa.h>

#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"
#include "plugins/Vst3RealtimeBridgeProtocol.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <pthread/qos.h>
#include <thread>

#if defined(NEURACOUST_HAS_VST3_SDK)
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <limits.h>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#endif

using namespace neuracoust::daw;

#if defined(NEURACOUST_HAS_VST3_SDK)
class MemoryVst3Stream final : public Steinberg::IBStream {
public:
    MemoryVst3Stream() { FUNKNOWN_CTOR }
    ~MemoryVst3Stream() noexcept { FUNKNOWN_DTOR }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::IBStream)
        QUERY_INTERFACE(iid, obj, Steinberg::IBStream::iid, Steinberg::IBStream)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }

    Steinberg::uint32 PLUGIN_API release() override {
        if (Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return __funknownRefCount;
    }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (buffer == nullptr || numBytes < 0) {
            return Steinberg::kInvalidArgument;
        }
        const auto available = position_ < data_.size() ? data_.size() - position_ : 0;
        const auto count = std::min<size_t>(available, static_cast<size_t>(numBytes));
        if (count > 0) {
            std::memcpy(buffer, data_.data() + position_, count);
            position_ += count;
        }
        if (numBytesRead != nullptr) {
            *numBytesRead = static_cast<Steinberg::int32>(count);
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (buffer == nullptr || numBytes < 0) {
            return Steinberg::kInvalidArgument;
        }
        const auto count = static_cast<size_t>(numBytes);
        if (position_ + count > data_.size()) {
            data_.resize(position_ + count);
        }
        if (count > 0) {
            std::memcpy(data_.data() + position_, buffer, count);
            position_ += count;
        }
        if (numBytesWritten != nullptr) {
            *numBytesWritten = numBytes;
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override {
        Steinberg::int64 next = 0;
        if (mode == Steinberg::IBStream::kIBSeekSet) {
            next = pos;
        } else if (mode == Steinberg::IBStream::kIBSeekCur) {
            next = static_cast<Steinberg::int64>(position_) + pos;
        } else if (mode == Steinberg::IBStream::kIBSeekEnd) {
            next = static_cast<Steinberg::int64>(data_.size()) + pos;
        } else {
            return Steinberg::kInvalidArgument;
        }
        if (next < 0) {
            return Steinberg::kInvalidArgument;
        }
        position_ = static_cast<size_t>(next);
        if (result != nullptr) {
            *result = static_cast<Steinberg::int64>(position_);
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (pos == nullptr) {
            return Steinberg::kInvalidArgument;
        }
        *pos = static_cast<Steinberg::int64>(position_);
        return Steinberg::kResultOk;
    }

    bool empty() const { return data_.empty(); }
    const std::vector<char>& data() const { return data_; }
    void setData(std::vector<char> data) {
        data_ = std::move(data);
        position_ = 0;
    }
    void rewind() { position_ = 0; }

private:
    Steinberg::int32 __funknownRefCount = 1;
    std::vector<char> data_;
    size_t position_ = 0;
};

class EditorParamValueQueue final : public Steinberg::Vst::IParamValueQueue {
public:
    explicit EditorParamValueQueue(Steinberg::Vst::ParamID parameterId) : parameterId_(parameterId) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IParamValueQueue)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IParamValueQueue::iid, Steinberg::Vst::IParamValueQueue)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return parameterId_; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return static_cast<Steinberg::int32>(points_.size()); }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index,
                                           Steinberg::int32& sampleOffset,
                                           Steinberg::Vst::ParamValue& value) override {
        if (index < 0 || static_cast<size_t>(index) >= points_.size()) {
            return Steinberg::kInvalidArgument;
        }
        sampleOffset = points_[static_cast<size_t>(index)].first;
        value = points_[static_cast<size_t>(index)].second;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override {
        points_.push_back({sampleOffset, std::clamp(value, 0.0, 1.0)});
        index = static_cast<Steinberg::int32>(points_.size() - 1u);
        return Steinberg::kResultOk;
    }

private:
    Steinberg::Vst::ParamID parameterId_;
    std::vector<std::pair<Steinberg::int32, Steinberg::Vst::ParamValue>> points_;
};

class EditorParameterChanges final : public Steinberg::Vst::IParameterChanges {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IParameterChanges)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IParameterChanges::iid, Steinberg::Vst::IParameterChanges)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return static_cast<Steinberg::int32>(queues_.size()); }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
        if (index < 0 || static_cast<size_t>(index) >= queues_.size()) {
            return nullptr;
        }
        return queues_[static_cast<size_t>(index)].get();
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id,
                                                                  Steinberg::int32& index) override {
        for (size_t queueIndex = 0; queueIndex < queues_.size(); ++queueIndex) {
            if (queues_[queueIndex]->getParameterId() == id) {
                index = static_cast<Steinberg::int32>(queueIndex);
                return queues_[queueIndex].get();
            }
        }
        queues_.push_back(std::make_unique<EditorParamValueQueue>(id));
        index = static_cast<Steinberg::int32>(queues_.size() - 1u);
        return queues_.back().get();
    }
    void addControllerSnapshot(Steinberg::Vst::IEditController* controller) {
        if (controller == nullptr) {
            return;
        }
        const int32_t count = std::min<int32_t>(128, std::max<int32_t>(0, controller->getParameterCount()));
        for (int32_t parameterIndex = 0; parameterIndex < count; ++parameterIndex) {
            Steinberg::Vst::ParameterInfo info {};
            if (controller->getParameterInfo(parameterIndex, info) != Steinberg::kResultOk ||
                (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0) {
                continue;
            }
            Steinberg::int32 queueIndex = 0;
            auto* queue = addParameterData(info.id, queueIndex);
            if (queue == nullptr) {
                continue;
            }
            Steinberg::int32 pointIndex = 0;
            queue->addPoint(0, controller->getParamNormalized(info.id), pointIndex);
        }
    }
    std::vector<std::pair<uint32_t, double>> latestValues() {
        std::vector<std::pair<uint32_t, double>> values;
        values.reserve(queues_.size());
        for (const auto& queue : queues_) {
            if (!queue) {
                continue;
            }
            Steinberg::int32 sampleOffset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPointCount() <= 0 ||
                queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) != Steinberg::kResultOk) {
                continue;
            }
            values.push_back({static_cast<uint32_t>(queue->getParameterId()), std::clamp(value, 0.0, 1.0)});
        }
        return values;
    }

private:
    std::vector<std::unique_ptr<EditorParamValueQueue>> queues_;
};

class HostAttributeList final : public Steinberg::Vst::IAttributeList {
public:
    HostAttributeList() { FUNKNOWN_CTOR }
    ~HostAttributeList() noexcept { FUNKNOWN_DTOR }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IAttributeList)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IAttributeList::iid, Steinberg::Vst::IAttributeList)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }

    Steinberg::uint32 PLUGIN_API release() override {
        if (Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return __funknownRefCount;
    }

    Steinberg::tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) override {
        ints_[id != nullptr ? id : ""] = value;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getInt(AttrID id, Steinberg::int64& value) override {
        const auto it = ints_.find(id != nullptr ? id : "");
        if (it == ints_.end()) {
            return Steinberg::kResultFalse;
        }
        value = it->second;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setFloat(AttrID id, double value) override {
        floats_[id != nullptr ? id : ""] = value;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        const auto it = floats_.find(id != nullptr ? id : "");
        if (it == floats_.end()) {
            return Steinberg::kResultFalse;
        }
        value = it->second;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setString(AttrID id, const Steinberg::Vst::TChar* string) override {
        std::vector<Steinberg::Vst::TChar> value;
        if (string != nullptr) {
            for (size_t index = 0; string[index] != 0; ++index) {
                value.push_back(string[index]);
            }
        }
        value.push_back(0);
        strings_[id != nullptr ? id : ""] = std::move(value);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getString(AttrID id, Steinberg::Vst::TChar* string, Steinberg::uint32 sizeInBytes) override {
        if (string == nullptr || sizeInBytes < sizeof(Steinberg::Vst::TChar)) {
            return Steinberg::kInvalidArgument;
        }
        const auto it = strings_.find(id != nullptr ? id : "");
        if (it == strings_.end()) {
            string[0] = 0;
            return Steinberg::kResultFalse;
        }
        const size_t capacity = sizeInBytes / sizeof(Steinberg::Vst::TChar);
        const size_t count = std::min(capacity, it->second.size());
        std::memcpy(string, it->second.data(), count * sizeof(Steinberg::Vst::TChar));
        string[capacity - 1] = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setBinary(AttrID id, const void* data, Steinberg::uint32 sizeInBytes) override {
        if (data == nullptr && sizeInBytes > 0) {
            return Steinberg::kInvalidArgument;
        }
        const auto* bytes = static_cast<const char*>(data);
        binaries_[id != nullptr ? id : ""] = std::vector<char>(bytes, bytes + sizeInBytes);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getBinary(AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) override {
        const auto it = binaries_.find(id != nullptr ? id : "");
        if (it == binaries_.end()) {
            data = nullptr;
            sizeInBytes = 0;
            return Steinberg::kResultFalse;
        }
        data = it->second.data();
        sizeInBytes = static_cast<Steinberg::uint32>(it->second.size());
        return Steinberg::kResultOk;
    }

private:
    Steinberg::int32 __funknownRefCount = 1;
    std::map<std::string, Steinberg::int64> ints_;
    std::map<std::string, double> floats_;
    std::map<std::string, std::vector<Steinberg::Vst::TChar>> strings_;
    std::map<std::string, std::vector<char>> binaries_;
};

class HostMessage final : public Steinberg::Vst::IMessage {
public:
    HostMessage() : attributes_(new HostAttributeList()) { FUNKNOWN_CTOR }
    ~HostMessage() noexcept {
        if (attributes_ != nullptr) {
            attributes_->release();
            attributes_ = nullptr;
        }
        FUNKNOWN_DTOR
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IMessage)
        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IMessage::iid, Steinberg::Vst::IMessage)
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }

    Steinberg::uint32 PLUGIN_API release() override {
        if (Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return __funknownRefCount;
    }

    Steinberg::FIDString PLUGIN_API getMessageID() override {
        return messageId_.empty() ? nullptr : messageId_.c_str();
    }

    void PLUGIN_API setMessageID(Steinberg::FIDString id) override {
        messageId_ = id != nullptr ? id : "";
    }

    Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes() override {
        attributes_->addRef();
        return attributes_;
    }

private:
    Steinberg::int32 __funknownRefCount = 1;
    std::string messageId_;
    HostAttributeList* attributes_ = nullptr;
};

@interface NAMeterTelemetryOverlayView : NSView
- (void)setInputLeft:(double)inputLeft inputRight:(double)inputRight outputLeft:(double)outputLeft outputRight:(double)outputRight;
@end

@implementation NAMeterTelemetryOverlayView {
    double inputLeft_;
    double inputRight_;
    double outputLeft_;
    double outputRight_;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 7.0;
        self.layer.masksToBounds = YES;
        self.layer.backgroundColor = [[NSColor colorWithCalibratedWhite:0.02 alpha:0.72] CGColor];
        inputLeft_ = inputRight_ = outputLeft_ = outputRight_ = 0.0;
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (void)setInputLeft:(double)inputLeft inputRight:(double)inputRight outputLeft:(double)outputLeft outputRight:(double)outputRight {
    inputLeft_ = std::max(0.0, std::min(1.0, inputLeft));
    inputRight_ = std::max(0.0, std::min(1.0, inputRight));
    outputLeft_ = std::max(0.0, std::min(1.0, outputLeft));
    outputRight_ = std::max(0.0, std::min(1.0, outputRight));
    self.hidden = NO;
    [self setNeedsDisplay:YES];
}

- (void)drawMeterPairInRect:(NSRect)rect left:(double)left right:(double)right color:(NSColor*)color {
    NSColor* slot = [NSColor colorWithCalibratedWhite:0.0 alpha:0.56];
    const CGFloat rowHeight = 5.0;
    const CGFloat rowGap = 3.0;
    NSArray<NSNumber*>* levels = @[@(left), @(right)];
    for (NSInteger row = 0; row < 2; ++row) {
        NSRect rowRect = NSMakeRect(rect.origin.x,
                                    rect.origin.y + row * (rowHeight + rowGap),
                                    rect.size.width,
                                    rowHeight);
        [NSGraphicsContext saveGraphicsState];
        [[NSBezierPath bezierPathWithRoundedRect:rowRect xRadius:2.0 yRadius:2.0] setClip];
        [slot setFill];
        NSRectFill(rowRect);
        const CGFloat width = rowRect.size.width * std::max(0.0, std::min(1.0, levels[row].doubleValue));
        if (width > 0.5) {
            NSRect fillRect = rowRect;
            fillRect.size.width = width;
            [color setFill];
            NSRectFill(fillRect);
            if (levels[row].doubleValue > 0.86) {
                NSRect warnRect = fillRect;
                warnRect.origin.x = NSMaxX(fillRect) - std::min<CGFloat>(8.0, fillRect.size.width);
                warnRect.size.width = std::min<CGFloat>(8.0, fillRect.size.width);
                [[NSColor colorWithCalibratedRed:0.95 green:0.72 blue:0.25 alpha:0.92] setFill];
                NSRectFill(warnRect);
            }
        }
        [NSGraphicsContext restoreGraphicsState];
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [[NSColor colorWithCalibratedWhite:0.02 alpha:0.72] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:7.0 yRadius:7.0] fill];
    NSDictionary* labelAttributes = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:8.0 weight:NSFontWeightBold],
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.88 alpha:0.92]
    };
    [@"IN" drawInRect:NSMakeRect(8, 5, 26, 11) withAttributes:labelAttributes];
    [@"OUT" drawInRect:NSMakeRect(self.bounds.size.width - 126, 5, 26, 11) withAttributes:labelAttributes];
    [self drawMeterPairInRect:NSMakeRect(54, 5, 114, 13)
                         left:inputLeft_
                        right:inputRight_
                        color:[NSColor colorWithCalibratedRed:0.17 green:0.80 blue:0.66 alpha:0.96]];
    [self drawMeterPairInRect:NSMakeRect(self.bounds.size.width - 96, 5, 88, 13)
                         left:outputLeft_
                        right:outputRight_
                        color:[NSColor colorWithCalibratedRed:0.36 green:0.68 blue:0.98 alpha:0.96]];
}

@end
#endif

namespace {

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

std::string directoryName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash);
}

std::string vst3ResourceDirectory(const std::string& bundlePath) {
    if (bundlePath.empty()) {
        return {};
    }
    NSString* resources = [[NSString stringWithUTF8String:bundlePath.c_str()] stringByAppendingPathComponent:@"Contents/Resources"];
    BOOL isDirectory = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:resources isDirectory:&isDirectory] && isDirectory) {
        return resources.UTF8String != nullptr ? resources.UTF8String : "";
    }
    return {};
}

std::string izotopeCoreResourceDirectory(const std::string& bundlePath) {
    if (bundlePath.empty()) {
        return {};
    }
    NSString* bundle = [NSString stringWithUTF8String:bundlePath.c_str()];
    NSString* corePathFile = [[bundle stringByAppendingPathComponent:@"Contents/Resources"] stringByAppendingPathComponent:@"iZCore.path"];
    NSString* coreName = [NSString stringWithContentsOfFile:corePathFile encoding:NSUTF8StringEncoding error:nil];
    if (coreName.length == 0) {
        return {};
    }
    coreName = [coreName stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (coreName.length == 0) {
        return {};
    }
    NSArray<NSString*>* candidates = @[
        [[[bundle stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Cores"] stringByAppendingPathComponent:coreName],
        [[@"/Library/Application Support/iZotope/Ozone 12/Cores" stringByAppendingPathComponent:coreName] stringByStandardizingPath],
        [[@"/Library/Application Support/iZotope/RX 12/Cores" stringByAppendingPathComponent:coreName] stringByStandardizingPath]
    ];
    NSFileManager* manager = [NSFileManager defaultManager];
    for (NSString* candidate in candidates) {
        NSString* resources = [candidate stringByAppendingPathComponent:@"Contents/Resources"];
        BOOL isDirectory = NO;
        if ([manager fileExistsAtPath:resources isDirectory:&isDirectory] && isDirectory) {
            return resources.UTF8String != nullptr ? resources.UTF8String : "";
        }
    }
    return {};
}

std::string lowercaseAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool setPreferredEditorMeterBusArrangement(Steinberg::Vst::IAudioProcessor* processor,
                                           int inputBusCount,
                                           int outputBusCount,
                                           int& inputChannelCount,
                                           int& outputChannelCount) {
    if (processor == nullptr) {
        return false;
    }
    auto trySetArrangement = [&](Steinberg::Vst::SpeakerArrangement inputArrangement,
                                 int candidateInputChannelCount,
                                 Steinberg::Vst::SpeakerArrangement outputArrangement,
                                 int candidateOutputChannelCount) {
        std::vector<Steinberg::Vst::SpeakerArrangement> inputArrangements(static_cast<size_t>(std::max(0, inputBusCount)),
                                                                          inputArrangement);
        std::vector<Steinberg::Vst::SpeakerArrangement> outputArrangements(static_cast<size_t>(std::max(0, outputBusCount)),
                                                                           outputArrangement);
        if (processor->setBusArrangements(inputArrangements.empty() ? nullptr : inputArrangements.data(),
                                          static_cast<Steinberg::int32>(inputArrangements.size()),
                                          outputArrangements.empty() ? nullptr : outputArrangements.data(),
                                          static_cast<Steinberg::int32>(outputArrangements.size())) != Steinberg::kResultOk) {
            return false;
        }
        inputChannelCount = inputBusCount > 0 ? candidateInputChannelCount : 0;
        outputChannelCount = outputBusCount > 0 ? candidateOutputChannelCount : 0;
        return true;
    };
    return trySetArrangement(Steinberg::Vst::SpeakerArr::kStereo, 2, Steinberg::Vst::SpeakerArr::kStereo, 2) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kMono, 1, Steinberg::Vst::SpeakerArr::kMono, 1) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kMono, 1, Steinberg::Vst::SpeakerArr::kStereo, 2) ||
           trySetArrangement(Steinberg::Vst::SpeakerArr::kStereo, 2, Steinberg::Vst::SpeakerArr::kMono, 1);
}

bool shouldUseResourceOverlayForDescriptor(const Vst3PluginDescriptor& descriptor) {
    if (std::getenv("NEURACOUST_FORCE_VST3_RESOURCE_OVERLAY") != nullptr) {
        return true;
    }
    if (std::getenv("NEURACOUST_DISABLE_VST3_RESOURCE_OVERLAY") != nullptr) {
        return false;
    }
    const std::string combined = lowercaseAscii(descriptor.brand + " " +
                                                descriptor.vendor + " " +
                                                descriptor.name + " " +
                                                descriptor.bundlePath);
    if (combined.find("waveshell") != std::string::npos || combined.find("waves ") != std::string::npos) {
        return false;
    }
    if (combined.find("fabfilter") != std::string::npos) {
        return false;
    }
    return true;
}

bool shouldSkipComponentConnectionForEditor(const Vst3PluginDescriptor& descriptor) {
    if (std::getenv("NEURACOUST_VST3_EDITOR_FORCE_COMPONENT_CONNECTION") != nullptr) {
        return false;
    }
    const std::string combined = lowercaseAscii(descriptor.brand + " " +
                                                descriptor.vendor + " " +
                                                descriptor.name + " " +
                                                descriptor.bundlePath + " " +
                                                descriptor.componentClassName);
    return combined.find("izotope") != std::string::npos ||
        combined.find("ozone") != std::string::npos ||
        combined.find("rx ") != std::string::npos ||
        combined.find("rx-") != std::string::npos;
}

#if defined(NEURACOUST_HAS_VST3_SDK)
std::string tuidToHex(const Steinberg::TUID cid) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int index = 0; index < 16; ++index) {
        out << std::setw(2) << (static_cast<int>(cid[index]) & 0xff);
    }
    return out.str();
}

std::string normalizedClassId(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return ch == '-' || ch == ' ' || ch == '{' || ch == '}';
    }), text.end());
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool classMatchesDescriptor(const Steinberg::PClassInfo& info,
                            const Vst3PluginDescriptor& descriptor) {
    if (!descriptor.componentClassCid.empty() &&
        normalizedClassId(descriptor.componentClassCid) == normalizedClassId(tuidToHex(info.cid))) {
        return true;
    }
    if (!descriptor.componentClassName.empty() && descriptor.componentClassName == info.name) {
        return true;
    }
    if (!descriptor.name.empty() && descriptor.name == info.name) {
        return true;
    }
    return false;
}

std::string classInfoSummary(const Steinberg::PClassInfo& info) {
    std::ostringstream out;
    out << info.name << " [" << info.category << "] " << tuidToHex(info.cid);
    return out.str();
}

std::string printableStatePreview(const std::vector<char>& data, size_t limit = 512) {
    std::ostringstream out;
    const size_t count = std::min(limit, data.size());
    for (size_t index = 0; index < count; ++index) {
        const unsigned char ch = static_cast<unsigned char>(data[index]);
        if (ch >= 32 && ch <= 126) {
            out << static_cast<char>(ch);
        } else if (ch == '\n') {
            out << "\\n";
        } else if (ch == '\r') {
            out << "\\r";
        } else if (ch == '\t') {
            out << "\\t";
        } else {
            out << '.';
        }
    }
    return out.str();
}

std::string trimAscii(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
        return !isSpace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
        return !isSpace(ch);
    }).base(), value.end());
    return value;
}

std::string readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path wavesProductBundleForName(const std::string& productName) {
    if (productName.empty()) {
        return {};
    }
    const std::string wanted = lowercaseAscii(productName);
    const std::vector<std::filesystem::path> roots = {
        "/Applications/Waves/Plug-Ins V16",
        "/Applications/Waves/Plug-Ins V15",
        "/Applications/Waves/Plug-Ins V14"
    };
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_directory() || entry.path().extension() != ".bundle") {
                continue;
            }
            std::string candidate = entry.path().stem().string();
            const std::string manifest = readSmallTextFile(entry.path() / "Contents" / "manifest.yaml");
            const auto namePos = manifest.find("name:");
            if (namePos != std::string::npos) {
                const auto lineEnd = manifest.find('\n', namePos);
                candidate = trimAscii(manifest.substr(namePos + 5, lineEnd == std::string::npos ? std::string::npos : lineEnd - (namePos + 5)));
            }
            if (lowercaseAscii(candidate) == wanted || lowercaseAscii(entry.path().stem().string()) == wanted) {
                return entry.path();
            }
        }
    }
    return {};
}

std::string firstRegexLikeBetween(const std::string& text,
                                  const std::string& prefix,
                                  const std::string& suffix) {
    const auto begin = text.find(prefix);
    if (begin == std::string::npos) {
        return {};
    }
    const auto valueBegin = begin + prefix.size();
    const auto end = text.find(suffix, valueBegin);
    if (end == std::string::npos) {
        return {};
    }
    return text.substr(valueBegin, end - valueBegin);
}

std::string wavesProductGenericType(const std::filesystem::path& bundlePath) {
    const std::string presets = readSmallTextFile(bundlePath / "Contents" / "Resources" / "Presets" / "1000.xml");
    return trimAscii(firstRegexLikeBetween(presets, "GenericType=\"", "\""));
}

std::string wavesProductSubComponent(const std::filesystem::path& bundlePath, const std::string& productName, bool stereo) {
    const std::string process = readSmallTextFile(bundlePath / "Contents" / "Resources" / "ProcessXML" / "1001.xml");
    std::vector<std::string> candidates;
    size_t cursor = 0;
    while (true) {
        const auto tag = process.find("<SubComponentType", cursor);
        if (tag == std::string::npos) {
            break;
        }
        const auto tagEnd = process.find("</SubComponentType>", tag);
        if (tagEnd == std::string::npos) {
            break;
        }
        const std::string tagText = process.substr(tag, tagEnd - tag);
        if (productName.empty() || tagText.find(productName) != std::string::npos) {
            const auto quoteBegin = tagText.find('\'');
            const auto quoteEnd = quoteBegin == std::string::npos ? std::string::npos : tagText.find('\'', quoteBegin + 1);
            if (quoteBegin != std::string::npos && quoteEnd != std::string::npos && quoteEnd > quoteBegin + 1) {
                candidates.push_back(tagText.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1));
            }
        }
        cursor = tagEnd + 1;
    }
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && ((stereo && candidate.back() == 'S') || (!stereo && candidate.back() == 'M'))) {
            return candidate;
        }
    }
    return candidates.empty() ? std::string() : candidates.front();
}

void writeBigEndian32(std::vector<char>& data, size_t offset, uint32_t value) {
    if (data.size() < offset + 4) {
        return;
    }
    data[offset + 0] = static_cast<char>((value >> 24) & 0xff);
    data[offset + 1] = static_cast<char>((value >> 16) & 0xff);
    data[offset + 2] = static_cast<char>((value >> 8) & 0xff);
    data[offset + 3] = static_cast<char>(value & 0xff);
}

bool replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return false;
    }
    bool changed = false;
    size_t cursor = 0;
    while ((cursor = text.find(from, cursor)) != std::string::npos) {
        text.replace(cursor, from.size(), to);
        cursor += to.size();
        changed = true;
    }
    return changed;
}

std::vector<char> wavesStateForProduct(const std::vector<char>& defaultState,
                                       const std::string& productName,
                                       const std::string& componentClassName) {
    const auto bundlePath = wavesProductBundleForName(productName);
    if (bundlePath.empty()) {
        return {};
    }
    const bool stereo = componentClassName.find("Stereo") != std::string::npos;
    const std::string subComponent = wavesProductSubComponent(bundlePath, productName, stereo);
    const std::string genericType = wavesProductGenericType(bundlePath);
    if (subComponent.size() != 4 || genericType.empty()) {
        return {};
    }
    constexpr const char* kPresetChunkMarker = "<PresetChunkXMLTree";
    auto xmlBeginIt = std::search(defaultState.begin(), defaultState.end(),
                                  kPresetChunkMarker,
                                  kPresetChunkMarker + std::char_traits<char>::length(kPresetChunkMarker));
    if (xmlBeginIt == defaultState.end()) {
        return {};
    }
    const size_t xmlOffset = static_cast<size_t>(std::distance(defaultState.begin(), xmlBeginIt));
    std::string xml(xmlBeginIt, defaultState.end());
    replaceAll(xml, "GenericType=\"AMMW\"", "GenericType=\"" + genericType + "\"");
    replaceAll(xml, "<PluginName>Immersive Wrapper</PluginName>", "<PluginName>" + productName + "</PluginName>");
    replaceAll(xml, "<PluginSubComp>200I</PluginSubComp>", "<PluginSubComp>" + subComponent + "</PluginSubComp>");
    std::vector<char> patched(defaultState.begin(), defaultState.begin() + static_cast<std::ptrdiff_t>(xmlOffset));
    patched.insert(patched.end(), xml.begin(), xml.end());
    if (patched.size() >= 16) {
        std::copy(subComponent.begin(), subComponent.end(), patched.begin() + 12);
    }
    if (patched.size() >= 24) {
        writeBigEndian32(patched, 0, static_cast<uint32_t>(patched.size() - 34));
        writeBigEndian32(patched, 20, static_cast<uint32_t>(patched.size() - 62));
    }
    return patched;
}
#endif

std::string createResourceOverlayIfUseful(const std::string& resourcesDir) {
    if (resourcesDir.empty()) {
        return {};
    }
    namespace fs = std::filesystem;
    const fs::path source(resourcesDir);
    const fs::path graphics = source / "Graphics";
    if (!fs::exists(graphics) || !fs::is_directory(graphics)) {
        return {};
    }
    auto createSymlinkIfMissing = [](const fs::path& sourcePath, const fs::path& targetPath) {
        std::error_code linkError;
        if (sourcePath.empty() || targetPath.empty() || fs::exists(targetPath, linkError)) {
            return;
        }
        if (targetPath.has_parent_path()) {
            fs::create_directories(targetPath.parent_path(), linkError);
            linkError.clear();
        }
        fs::create_symlink(sourcePath, targetPath, linkError);
    };
    auto copyFileIfMissing = [](const fs::path& sourcePath, const fs::path& targetPath) {
        std::error_code copyError;
        if (sourcePath.empty() || targetPath.empty() || fs::exists(targetPath, copyError)) {
            return;
        }
        if (targetPath.has_parent_path()) {
            fs::create_directories(targetPath.parent_path(), copyError);
            copyError.clear();
        }
        fs::copy_file(sourcePath, targetPath, fs::copy_options::overwrite_existing, copyError);
    };

    bool hasGraphicsResources = false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(graphics, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file()) {
            hasGraphicsResources = true;
            break;
        }
    }
    if (!hasGraphicsResources) {
        return {};
    }
    NSString* tempRoot = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"neuracoust-vst3-resources-%@",
                                                                                  [[NSUUID UUID] UUIDString]]];
    const fs::path overlay(tempRoot.UTF8String != nullptr ? tempRoot.UTF8String : "");
    if (overlay.empty()) {
        return {};
    }
    fs::create_directories(overlay, ec);
    if (ec) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        if (ec) {
            break;
        }
        const fs::path target = overlay / entry.path().filename();
        if (entry.path().filename() == "Graphics") {
            continue;
        }
        if (entry.is_directory()) {
            fs::create_directory_symlink(entry.path(), target, ec);
            if (ec) {
                ec.clear();
            }
        } else if (entry.is_regular_file()) {
            createSymlinkIfMissing(entry.path(), target);
        } else {
            fs::create_symlink(entry.path(), target, ec);
            if (ec) {
                ec.clear();
            }
        }
    }
    const fs::path overlayGraphics = overlay / "Graphics";
    fs::create_directories(overlayGraphics, ec);
    if (ec) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(graphics, ec)) {
        if (ec) {
            break;
        }
        const fs::path graphicsTarget = overlayGraphics / entry.path().filename();
        if (entry.is_directory()) {
            fs::create_directory_symlink(entry.path(), graphicsTarget, ec);
            if (ec) {
                ec.clear();
            }
            continue;
        }
        createSymlinkIfMissing(entry.path(), graphicsTarget);
        if (entry.is_regular_file()) {
            createSymlinkIfMissing(entry.path(), overlay / entry.path().filename());
        }
    }
    for (const auto& entry : fs::directory_iterator(graphics, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        const std::string suffix = "_2x.png";
        if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix) {
            continue;
        }
        const std::string baseName = name.substr(0, name.size() - suffix.size()) + ".png";
        const fs::path alias = overlayGraphics / baseName;
        createSymlinkIfMissing(entry.path(), alias);
        const fs::path rootAlias = overlay / baseName;
        createSymlinkIfMissing(entry.path(), rootAlias);
    }
    return overlay.string();
}

std::string createBundleOverlayIfUseful(const std::string& bundlePath, const std::string& resourcesOverlayDir, std::string& cleanupDirectory) {
    if (bundlePath.empty() || resourcesOverlayDir.empty()) {
        return {};
    }
    namespace fs = std::filesystem;
    const fs::path sourceBundle(bundlePath);
    const fs::path sourceContents = sourceBundle / "Contents";
    if (!fs::exists(sourceContents) || !fs::is_directory(sourceContents)) {
        return {};
    }
    NSString* tempRootString = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"neuracoust-vst3-bundle-%@",
                                                                                        [[NSUUID UUID] UUIDString]]];
    const fs::path tempRoot(tempRootString.UTF8String != nullptr ? tempRootString.UTF8String : "");
    if (tempRoot.empty()) {
        return {};
    }
    const fs::path overlayBundle = tempRoot / sourceBundle.filename();
    const fs::path overlayContents = overlayBundle / "Contents";
    std::error_code ec;
    fs::create_directories(overlayContents, ec);
    if (ec) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(sourceContents, ec)) {
        if (ec) {
            break;
        }
        const fs::path name = entry.path().filename();
        const fs::path target = overlayContents / name;
        if (name == "_CodeSignature") {
            continue;
        }
        if (name == "Info.plist" && entry.is_regular_file()) {
            NSString* sourceInfoPath = [NSString stringWithUTF8String:entry.path().string().c_str()];
            NSMutableDictionary* info = [NSMutableDictionary dictionaryWithContentsOfFile:sourceInfoPath];
            if (info != nil) {
                [info removeObjectForKey:@"CSResourcesFileMapped"];
                NSString* targetInfoPath = [NSString stringWithUTF8String:target.string().c_str()];
                [info writeToFile:targetInfoPath atomically:YES];
            } else {
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
            }
        } else if (name == "Resources") {
            fs::create_directories(target, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            for (const auto& resourceEntry : fs::directory_iterator(resourcesOverlayDir, ec)) {
                if (ec) {
                    break;
                }
                const fs::path resourceTarget = target / resourceEntry.path().filename();
                if (resourceEntry.is_directory()) {
                    fs::create_directory_symlink(resourceEntry.path(), resourceTarget, ec);
                } else if (resourceEntry.is_regular_file()) {
                    fs::copy_file(resourceEntry.path(), resourceTarget, fs::copy_options::overwrite_existing, ec);
                } else {
                    fs::create_symlink(resourceEntry.path(), resourceTarget, ec);
                }
                if (ec) {
                    ec.clear();
                }
            }
        } else if (name == "MacOS" && entry.is_directory()) {
            fs::create_directories(target, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            for (const auto& executableEntry : fs::directory_iterator(entry.path(), ec)) {
                if (ec) {
                    break;
                }
                const fs::path executableTarget = target / executableEntry.path().filename();
                if (executableEntry.is_directory()) {
                    fs::create_directory_symlink(executableEntry.path(), executableTarget, ec);
                } else if (executableEntry.is_regular_file()) {
                    fs::copy_file(executableEntry.path(), executableTarget, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        fs::permissions(executableTarget,
                                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                        fs::perm_options::add,
                                        ec);
                    }
                } else {
                    fs::create_symlink(executableEntry.path(), executableTarget, ec);
                }
                if (ec) {
                    ec.clear();
                }
            }
        } else if (entry.is_directory()) {
            fs::create_directory_symlink(entry.path(), target, ec);
        } else {
            fs::create_symlink(entry.path(), target, ec);
        }
        if (ec) {
            ec.clear();
        }
    }
    cleanupDirectory = tempRoot.string();
    return overlayBundle.string();
}

bool isStaleNeuracoustVstResourceLink(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_symlink(fs::symlink_status(path, ec))) {
        return false;
    }
    const fs::path target = fs::read_symlink(path, ec);
    if (ec) {
        return false;
    }
    return target.string().find("neuracoust-vst3-resources-") != std::string::npos;
}

void cleanupStaleMirroredResourcesInMainBundle() {
    NSString* resourcePathString = [[NSBundle mainBundle] resourcePath];
    if (resourcePathString.length == 0) {
        return;
    }
    namespace fs = std::filesystem;
    const fs::path destination(resourcePathString.UTF8String != nullptr ? resourcePathString.UTF8String : "");
    if (destination.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::exists(destination, ec) || !fs::is_directory(destination, ec)) {
        return;
    }
    auto cleanDirectory = [&](const fs::path& directory) {
        std::error_code iteratorError;
        for (const auto& entry : fs::directory_iterator(directory, iteratorError)) {
            if (iteratorError) {
                break;
            }
            if (isStaleNeuracoustVstResourceLink(entry.path())) {
                std::error_code removeError;
                fs::remove(entry.path(), removeError);
            }
        }
    };
    cleanDirectory(destination);
    const fs::path graphics = destination / "Graphics";
    if (fs::exists(graphics, ec) && fs::is_directory(graphics, ec)) {
        cleanDirectory(graphics);
    }
}

void mirrorResourceOverlayIntoMainBundle(const std::string& resourcesOverlayDir, std::vector<std::filesystem::path>& createdPaths) {
    if (resourcesOverlayDir.empty()) {
        return;
    }
    cleanupStaleMirroredResourcesInMainBundle();
    NSString* resourcePathString = [[NSBundle mainBundle] resourcePath];
    if (resourcePathString.length == 0) {
        return;
    }
    namespace fs = std::filesystem;
    const fs::path source(resourcesOverlayDir);
    const fs::path destination(resourcePathString.UTF8String != nullptr ? resourcePathString.UTF8String : "");
    if (source.empty() || destination.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::exists(destination, ec) || !fs::is_directory(destination, ec)) {
        return;
    }
    auto linkIfMissing = [&](const fs::path& sourcePath, const fs::path& targetPath) {
        std::error_code linkError;
        if (sourcePath.empty() || targetPath.empty()) {
            return;
        }
        if (isStaleNeuracoustVstResourceLink(targetPath)) {
            fs::remove(targetPath, linkError);
            linkError.clear();
        } else if (fs::exists(targetPath, linkError)) {
            return;
        }
        if (targetPath.has_parent_path()) {
            fs::create_directories(targetPath.parent_path(), linkError);
            linkError.clear();
        }
        fs::create_symlink(sourcePath, targetPath, linkError);
        if (!linkError) {
            createdPaths.push_back(targetPath);
        }
    };
    auto copyIfMissing = [&](const fs::path& sourcePath, const fs::path& targetPath) {
        std::error_code copyError;
        if (sourcePath.empty() || targetPath.empty()) {
            return;
        }
        if (isStaleNeuracoustVstResourceLink(targetPath)) {
            fs::remove(targetPath, copyError);
            copyError.clear();
        } else if (fs::exists(targetPath, copyError)) {
            return;
        }
        if (targetPath.has_parent_path()) {
            fs::create_directories(targetPath.parent_path(), copyError);
            copyError.clear();
        }
        fs::copy_file(sourcePath, targetPath, fs::copy_options::overwrite_existing, copyError);
        if (!copyError) {
            createdPaths.push_back(targetPath);
        }
    };
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        if (ec) {
            break;
        }
        const fs::path target = destination / entry.path().filename();
        if (entry.is_regular_file()) {
            copyIfMissing(entry.path(), target);
        } else if (entry.is_symlink()) {
            linkIfMissing(entry.path(), target);
        }
    }
    const fs::path sourceGraphics = source / "Graphics";
    if (fs::exists(sourceGraphics, ec) && fs::is_directory(sourceGraphics, ec)) {
        const fs::path destinationGraphics = destination / "Graphics";
        fs::create_directories(destinationGraphics, ec);
        ec.clear();
        for (const auto& entry : fs::directory_iterator(sourceGraphics, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file() || entry.is_symlink()) {
                linkIfMissing(entry.path(), destinationGraphics / entry.path().filename());
            }
        }
    }
}

std::string currentWorkingDirectory() {
    char buffer[PATH_MAX] = {};
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

void showFatalAlert(NSString* title, NSString* message) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = message;
    [alert addButtonWithTitle:@"닫기"];
    [alert runModal];
    [alert release];
}

#if defined(NEURACOUST_HAS_VST3_SDK)
class Vst3EditorSession;

void logHostStage(const char* stage) {
    std::cout << "HOST_STAGE " << stage << " cwd=" << currentWorkingDirectory() << std::endl;
}

void logHostStage(const std::string& stage) {
    logHostStage(stage.c_str());
}

Steinberg::ViewRect normalizedViewRect(const Steinberg::ViewRect& rect) {
    const Steinberg::int32 width = std::max<Steinberg::int32>(1, rect.getWidth());
    const Steinberg::int32 height = std::max<Steinberg::int32>(1, rect.getHeight());
    return Steinberg::ViewRect(0, 0, width, height);
}

bool shouldForwardPolledParameterValue(const std::optional<double>& previousValue,
                                       double currentValue) {
    if (!std::isfinite(currentValue)) {
        return false;
    }
    return !previousValue.has_value() ||
        std::fabs(*previousValue - currentValue) >= 0.000001;
}

int runParameterPollingSelfTest() {
    if (!shouldForwardPolledParameterValue(std::nullopt, 0.5)) {
        std::cerr << "PARAM_POLL_SELF_TEST failed: initial value was not forwarded\n";
        return 21;
    }
    if (shouldForwardPolledParameterValue(0.5, 0.5)) {
        std::cerr << "PARAM_POLL_SELF_TEST failed: unchanged value was forwarded\n";
        return 22;
    }
    if (shouldForwardPolledParameterValue(0.5, 0.5000004)) {
        std::cerr << "PARAM_POLL_SELF_TEST failed: epsilon jitter was forwarded\n";
        return 23;
    }
    if (!shouldForwardPolledParameterValue(0.5, 0.500002)) {
        std::cerr << "PARAM_POLL_SELF_TEST failed: real edit was suppressed\n";
        return 24;
    }
    if (shouldForwardPolledParameterValue(0.5, std::numeric_limits<double>::quiet_NaN())) {
        std::cerr << "PARAM_POLL_SELF_TEST failed: non-finite value was forwarded\n";
        return 25;
    }
    std::cout << "PARAM_POLL_SELF_TEST ok\n";
    return 0;
}

Steinberg::ViewRect chooseWavesViewRectCandidateForSubview(const Steinberg::ViewRect& pluginRect,
                                                          const std::optional<Steinberg::ViewRect>& subviewRect,
                                                          Steinberg::int32 minimumWidth,
                                                          Steinberg::int32 minimumHeight,
                                                          bool* usedSubview = nullptr) {
    Steinberg::ViewRect candidate = normalizedViewRect(pluginRect);
    if (usedSubview != nullptr) {
        *usedSubview = false;
    }
    if (!subviewRect) {
        return candidate;
    }
    const Steinberg::ViewRect normalizedSubview = normalizedViewRect(*subviewRect);
    const int widthDelta = std::abs(normalizedSubview.getWidth() - candidate.getWidth());
    const int heightDelta = std::abs(normalizedSubview.getHeight() - candidate.getHeight());
    if (widthDelta < 8 && heightDelta < 8) {
        return candidate;
    }
    if (usedSubview != nullptr) {
        *usedSubview = true;
    }
    return Steinberg::ViewRect(0,
                               0,
                               std::max<Steinberg::int32>(minimumWidth, normalizedSubview.getWidth()),
                               std::max<Steinberg::int32>(minimumHeight, normalizedSubview.getHeight()));
}

Steinberg::ViewRect normalizedFlexibleWavesRectForState(const Steinberg::ViewRect& rect,
                                                        bool flexibleWavesRs124,
                                                        bool initialSizeCapActive,
                                                        Steinberg::int32 initialSizeCapHeight) {
    Steinberg::ViewRect normalized = normalizedViewRect(rect);
    if (!flexibleWavesRs124) {
        return normalized;
    }
    const Steinberg::int32 width = std::max<Steinberg::int32>(1, normalized.getWidth());
    if (initialSizeCapActive &&
        initialSizeCapHeight > 0 &&
        normalized.getHeight() > initialSizeCapHeight) {
        return Steinberg::ViewRect(0, 0, width, initialSizeCapHeight);
    }
    // The RS124 has a collapse button that toggles a two- vs one-unit layout, so its
    // height is snapped to those two states. But the Waves GUI Size menu (125/150/200%)
    // scales the whole view uniformly — width and height together. Snapping height to a
    // fixed 365/540 while width grew clipped the panels off the bottom on enlarge.
    // Derive the scale from the width (native is 840) so the two snap heights scale too.
    constexpr double kNativeRs124Width = 840.0;
    const double scale = std::max(0.25, static_cast<double>(width) / kNativeRs124Width);
    const Steinberg::int32 threshold = static_cast<Steinberg::int32>(std::lround(430.0 * scale));
    const Steinberg::int32 collapsedHeight = static_cast<Steinberg::int32>(std::lround(365.0 * scale));
    const Steinberg::int32 expandedHeight = static_cast<Steinberg::int32>(std::lround(540.0 * scale));
    const Steinberg::int32 height = normalized.getHeight() <= threshold ? collapsedHeight : expandedHeight;
    return Steinberg::ViewRect(0, 0, width, height);
}

std::optional<Steinberg::ViewRect> initialFlexibleWavesCollapsedRect(const Vst3PluginDescriptor& descriptor,
                                                                     const Steinberg::ViewRect& reportedRect) {
    const std::string identity = lowercaseAscii(descriptor.brand + " " +
                                                descriptor.vendor + " " +
                                                descriptor.name + " " +
                                                descriptor.componentClassName + " " +
                                                descriptor.bundlePath);
    if (identity.find("waves") == std::string::npos &&
        identity.find("waveshell") == std::string::npos) {
        return std::nullopt;
    }
    const Steinberg::int32 width = std::max<Steinberg::int32>(1, reportedRect.getWidth());
    const Steinberg::int32 height = std::max<Steinberg::int32>(1, reportedRect.getHeight());
    Steinberg::int32 collapsedHeight = 0;
    if (identity.find("abbey road rs124") != std::string::npos && height > 470) {
        collapsedHeight = 365;
    } else if (identity.find("h-reverb") != std::string::npos && height > 520) {
        collapsedHeight = 355;
    }
    if (collapsedHeight <= 0 || collapsedHeight >= height) {
        return std::nullopt;
    }
    return Steinberg::ViewRect(0, 0, width, collapsedHeight);
}

int runWavesViewSizeSelfTest() {
    Vst3PluginDescriptor rs124;
    rs124.brand = "Waves";
    rs124.name = "Abbey Road RS124 Stereo";
    rs124.componentClassName = "Abbey Road RS124 Stereo";
    const auto rs124Collapsed = initialFlexibleWavesCollapsedRect(rs124, Steinberg::ViewRect(0, 0, 840, 610));
    if (!rs124Collapsed || rs124Collapsed->getWidth() != 840 || rs124Collapsed->getHeight() != 365) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: RS124 initial collapsed rect was not selected\n";
        return 31;
    }

    Vst3PluginDescriptor hReverb;
    hReverb.vendor = "Waves";
    hReverb.name = "H-Reverb Stereo";
    hReverb.componentClassName = "H-Reverb Stereo";
    const auto hReverbCollapsed = initialFlexibleWavesCollapsedRect(hReverb, Steinberg::ViewRect(0, 0, 656, 650));
    if (!hReverbCollapsed || hReverbCollapsed->getWidth() != 656 || hReverbCollapsed->getHeight() != 355) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: H-Reverb initial collapsed rect was not selected\n";
        return 32;
    }

    Vst3PluginDescriptor fabFilter;
    fabFilter.brand = "FabFilter";
    fabFilter.name = "Pro-L 2";
    if (initialFlexibleWavesCollapsedRect(fabFilter, Steinberg::ViewRect(0, 0, 800, 700)).has_value()) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: non-Waves product was collapsed\n";
        return 33;
    }

    bool usedSubview = false;
    const auto subviewCandidate = chooseWavesViewRectCandidateForSubview(Steinberg::ViewRect(0, 0, 388, 612),
                                                                         Steinberg::ViewRect(0, 0, 292, 492),
                                                                         160,
                                                                         120,
                                                                         &usedSubview);
    if (!usedSubview || subviewCandidate.getWidth() != 292 || subviewCandidate.getHeight() != 492) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: scaled subview candidate was not preferred\n";
        return 34;
    }

    usedSubview = false;
    const auto pluginCandidate = chooseWavesViewRectCandidateForSubview(Steinberg::ViewRect(0, 0, 388, 612),
                                                                        Steinberg::ViewRect(0, 0, 383, 608),
                                                                        160,
                                                                        120,
                                                                        &usedSubview);
    if (usedSubview || pluginCandidate.getWidth() != 388 || pluginCandidate.getHeight() != 612) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: near-identical subview overrode plug-in rect\n";
        return 35;
    }

    usedSubview = false;
    const auto dorroughScaledDown = chooseWavesViewRectCandidateForSubview(Steinberg::ViewRect(0, 0, 388, 612),
                                                                           Steinberg::ViewRect(0, 0, 320, 512),
                                                                           160,
                                                                           120,
                                                                           &usedSubview);
    if (!usedSubview || dorroughScaledDown.getWidth() != 320 || dorroughScaledDown.getHeight() != 512) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: scaled-down Waves view left stale host padding\n";
        return 36;
    }

    usedSubview = false;
    const auto dorroughScaledBackUp = chooseWavesViewRectCandidateForSubview(Steinberg::ViewRect(0, 0, 320, 512),
                                                                             Steinberg::ViewRect(0, 0, 388, 612),
                                                                             160,
                                                                             120,
                                                                             &usedSubview);
    if (!usedSubview || dorroughScaledBackUp.getWidth() != 388 || dorroughScaledBackUp.getHeight() != 612) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: scaled-up Waves view did not restore host bounds\n";
        return 37;
    }

    const auto rs124NormalizedCollapsed = normalizedFlexibleWavesRectForState(Steinberg::ViewRect(0, 0, 840, 480),
                                                                              true,
                                                                              true,
                                                                              365);
    if (rs124NormalizedCollapsed.getHeight() != 365) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: initial RS124 height cap was not applied\n";
        return 38;
    }
    const auto rs124NormalizedExpanded = normalizedFlexibleWavesRectForState(Steinberg::ViewRect(0, 0, 840, 500),
                                                                             true,
                                                                             false,
                                                                             365);
    if (rs124NormalizedExpanded.getHeight() != 540) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: expanded RS124 height was not normalized\n";
        return 39;
    }
    const auto rs124NormalizedRecollapsed = normalizedFlexibleWavesRectForState(Steinberg::ViewRect(0, 0, 840, 365),
                                                                                true,
                                                                                false,
                                                                                365);
    if (rs124NormalizedRecollapsed.getHeight() != 365) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: collapsed RS124 height was not restored after expand\n";
        return 40;
    }
    // GUI Size 150%: width grows to 1260, so the expanded height must scale with it
    // (≈810) rather than being clipped back to the native 540.
    const auto rs124Enlarged = normalizedFlexibleWavesRectForState(Steinberg::ViewRect(0, 0, 1260, 810),
                                                                   true,
                                                                   false,
                                                                   365);
    if (rs124Enlarged.getWidth() != 1260 || rs124Enlarged.getHeight() != 810) {
        std::cerr << "WAVES_VIEW_SIZE_SELF_TEST failed: enlarged RS124 view was clipped instead of scaled\n";
        return 41;
    }
    std::cout << "WAVES_VIEW_SIZE_SELF_TEST ok\n";
    return 0;
}

bool descriptorLooksLikeWavesRs124(const Vst3PluginDescriptor& descriptor) {
    const std::string identity = lowercaseAscii(descriptor.brand + " " +
                                                descriptor.vendor + " " +
                                                descriptor.name + " " +
                                                descriptor.componentClassName + " " +
                                                descriptor.bundlePath);
    return (identity.find("waves") != std::string::npos ||
            identity.find("waveshell") != std::string::npos) &&
        identity.find("abbey road rs124") != std::string::npos;
}

bool descriptorLooksLikeWavesProduct(const Vst3PluginDescriptor& descriptor) {
    const std::string identity = lowercaseAscii(descriptor.brand + " " +
                                                descriptor.vendor + " " +
                                                descriptor.name + " " +
                                                descriptor.componentClassName + " " +
                                                descriptor.bundlePath);
    return identity.find("waves") != std::string::npos ||
        identity.find("waveshell") != std::string::npos;
}

	bool descriptorShouldSuppressSyntheticEditorAudio(const Vst3PluginDescriptor& descriptor) {
	    const std::string identity = lowercaseAscii(descriptor.brand + " " +
	                                                descriptor.vendor + " " +
	                                                descriptor.name + " " +
                                                descriptor.componentClassName + " " +
                                                descriptor.bundlePath);
	    const bool analyzerOrMeter =
	        identity.find("geq") != std::string::npos ||
	        identity.find("rta") != std::string::npos ||
	        identity.find("analyzer") != std::string::npos ||
	        identity.find("spectrum") != std::string::npos ||
	        identity.find("dorrough") != std::string::npos ||
	        identity.find("meter") != std::string::npos;
	    if (analyzerOrMeter) {
	        return true;
	    }
	    if (std::getenv("NEURACOUST_VST3_EDITOR_ENABLE_SYNTHETIC_AUDIO") != nullptr) {
	        return false;
	    }
	    return true;
	}

	int runSyntheticEditorAudioPolicySelfTest() {
	    Vst3PluginDescriptor wavesGeq;
	    wavesGeq.name = "GEQ Modern";
	    wavesGeq.componentClassName = "GEQ Modern Stereo";
	    wavesGeq.bundlePath = "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 16.0.vst3";
	    if (!descriptorShouldSuppressSyntheticEditorAudio(wavesGeq)) {
	        std::cerr << "SYNTHETIC_AUDIO_POLICY_SELF_TEST failed: Waves GEQ was allowed to receive synthetic editor audio\n";
	        return 41;
	    }
	    Vst3PluginDescriptor genericAnalyzer;
	    genericAnalyzer.name = "Spectrum Analyzer";
	    genericAnalyzer.componentClassName = "Analyzer Stereo";
	    if (!descriptorShouldSuppressSyntheticEditorAudio(genericAnalyzer)) {
	        std::cerr << "SYNTHETIC_AUDIO_POLICY_SELF_TEST failed: analyzer was allowed to receive synthetic editor audio\n";
	        return 42;
	    }
	    Vst3PluginDescriptor genericEq;
	    genericEq.name = "Generic EQ";
	    genericEq.componentClassName = "Generic EQ Stereo";
	    if (!descriptorShouldSuppressSyntheticEditorAudio(genericEq)) {
	        std::cerr << "SYNTHETIC_AUDIO_POLICY_SELF_TEST failed: default editor synthetic audio policy was not suppressing\n";
	        return 43;
	    }
	    std::cout << "SYNTHETIC_AUDIO_POLICY_SELF_TEST ok\n";
	    return 0;
	}

std::string percentEncodeField(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char byte : value) {
        if (byte == '%' || byte == '\t' || byte == '\n' || byte == '\r') {
            encoded << '%' << static_cast<int>((byte >> 4) & 0x0F) << static_cast<int>(byte & 0x0F);
        } else {
            encoded << static_cast<char>(byte);
        }
    }
    return encoded.str();
}

int inspectParametersAndPrint(const Vst3PluginDescriptor& descriptor, int limit) {
    try {
        const Vst3ParameterInspection inspection = inspectVst3ParametersWithSdk(descriptor, limit);
        std::cout << "INSPECT"
                  << "\t" << (inspection.sdkAvailable ? 1 : 0)
                  << "\t" << (inspection.opened ? 1 : 0)
                  << "\t" << (inspection.hasFactory ? 1 : 0)
                  << "\t" << (inspection.controllerCreated ? 1 : 0)
                  << "\t" << (inspection.initialized ? 1 : 0)
                  << "\t" << inspection.parameterCount
                  << "\t" << percentEncodeField(inspection.className)
                  << "\t" << percentEncodeField(inspection.message)
                  << std::endl;
        for (const auto& parameter : inspection.parameters) {
            std::cout << "PARAMINFO"
                      << "\t" << parameter.id
                      << "\t" << parameter.defaultNormalized
                      << "\t" << parameter.currentNormalized
                      << "\t" << parameter.stepCount
                      << "\t" << parameter.flags
                      << "\t" << percentEncodeField(parameter.title)
                      << "\t" << percentEncodeField(parameter.units)
                      << "\t" << percentEncodeField(parameter.defaultDisplay)
                      << "\t" << percentEncodeField(parameter.currentDisplay)
                      << std::endl;
        }
        return inspection.parameters.empty() ? 8 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "INSPECT_ERROR " << exception.what() << std::endl;
        return 9;
    } catch (...) {
        std::cerr << "INSPECT_ERROR Unknown plug-in exception while inspecting parameters." << std::endl;
        return 10;
    }
}

class EditorHostSupport final : public Steinberg::Vst::IHostApplication,
                                public Steinberg::Vst::IComponentHandler,
                                public Steinberg::Vst::IPlugInterfaceSupport,
                                public Steinberg::IPlugFrame {
public:
    EditorHostSupport() { FUNKNOWN_CTOR }
    ~EditorHostSupport() noexcept { FUNKNOWN_DTOR }

		    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
		        QUERY_INTERFACE(iid, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IHostApplication)
		        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IHostApplication::iid, Steinberg::Vst::IHostApplication)
		        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IComponentHandler::iid, Steinberg::Vst::IComponentHandler)
		        QUERY_INTERFACE(iid, obj, Steinberg::Vst::IPlugInterfaceSupport::iid, Steinberg::Vst::IPlugInterfaceSupport)
		        QUERY_INTERFACE(iid, obj, Steinberg::IPlugFrame::iid, Steinberg::IPlugFrame)
		        *obj = nullptr;
		        return Steinberg::kNoInterface;
		    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }
    Steinberg::uint32 PLUGIN_API release() override {
        if (Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return __funknownRefCount;
    }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* appName = "Neuracoust VST3 Editor Host";
        int index = 0;
        for (; appName[index] != '\0' && index < 127; ++index) {
            name[index] = static_cast<Steinberg::Vst::TChar>(appName[index]);
        }
        name[index] = 0;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID iid, void** obj) override {
        if (obj == nullptr) {
            return Steinberg::kInvalidArgument;
        }
        *obj = nullptr;
        Steinberg::TUID messageTuid {};
        Steinberg::Vst::IMessage::iid.toTUID(messageTuid);
        Steinberg::TUID attributesTuid {};
        Steinberg::Vst::IAttributeList::iid.toTUID(attributesTuid);
        if (std::memcmp(cid, messageTuid, sizeof(Steinberg::TUID)) == 0 &&
            std::memcmp(iid, messageTuid, sizeof(Steinberg::TUID)) == 0) {
            *obj = new HostMessage();
            return Steinberg::kResultOk;
        }
        if (std::memcmp(cid, attributesTuid, sizeof(Steinberg::TUID)) == 0 &&
            std::memcmp(iid, attributesTuid, sizeof(Steinberg::TUID)) == 0) {
            *obj = new HostAttributeList();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }
    Steinberg::tresult PLUGIN_API isPlugInterfaceSupported(const Steinberg::TUID iid) override {
        Steinberg::TUID connectionTuid {};
        Steinberg::Vst::IConnectionPoint::iid.toTUID(connectionTuid);
        return std::memcmp(iid, connectionTuid, sizeof(Steinberg::TUID)) == 0
            ? Steinberg::kResultTrue
            : Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID parameterId, Steinberg::Vst::ParamValue value) override {
        if (parameterEditCallback) {
            parameterEditCallback(parameterId, std::clamp(value, 0.0, 1.0));
        }
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        if (componentRestartCallback) {
            componentRestartCallback(flags);
        }
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
	        if (panel == nil || view == nullptr || newSize == nullptr) {
	            return Steinberg::kInvalidArgument;
	        }
	        Steinberg::ViewRect constrained = *newSize;
	        if (!trustRequestedResize && view->canResize() == Steinberg::kResultTrue) {
	            view->checkSizeConstraint(&constrained);
	        }
            constrained = normalizedViewRect(constrained);
            if (viewResizedCallback) {
                viewResizedCallback(constrained);
                return Steinberg::kResultOk;
            }
	        const CGFloat width = std::max<CGFloat>(320.0, static_cast<CGFloat>(constrained.getWidth()));
	        const CGFloat height = std::max<CGFloat>(180.0, static_cast<CGFloat>(constrained.getHeight()));
	        [panel setContentSize:NSMakeSize(width, height)];
	        panel.contentView.frame = NSMakeRect(0, 0, width, height);
	        view->onSize(&constrained);
	        return Steinberg::kResultOk;
	    }

    NSPanel* panel = nil;
    bool trustRequestedResize = false;
    std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)> parameterEditCallback;
    std::function<void(Steinberg::int32)> componentRestartCallback;
    std::function<void(const Steinberg::ViewRect&)> viewResizedCallback;

private:
    Steinberg::int32 __funknownRefCount = 1;
};

class Vst3EditorSession {
public:
	    ~Vst3EditorSession() { close(); }

		    bool open(const Vst3PluginDescriptor& descriptor, NSString* title, bool meterOverlayEnabled, std::string& error) {
	                descriptor_ = descriptor;
	                wavesEditor_ = descriptorLooksLikeWavesProduct(descriptor);
	                flexibleWavesRs124_ = descriptorLooksLikeWavesRs124(descriptor);
	                suppressSyntheticEditorAudio_ = descriptorShouldSuppressSyntheticEditorAudio(descriptor);
	                meterOverlayEnabled_ = meterOverlayEnabled;
                trustRequestedResize_ = lowercaseAscii(descriptor.bundlePath + " " +
                                                       descriptor.vendor + " " +
                                                       descriptor.brand + " " +
                                                       descriptor.name + " " +
                                                       descriptor.componentClassName).find("waves") != std::string::npos;
                allowComponentStateTransfer_ = std::getenv("NEURACOUST_VST3_EDITOR_TRANSFER_COMPONENT_STATE") != nullptr;
	            logHostStage("open.begin");
	            std::string bundlePathForLoad = descriptor.bundlePath;
                if (!descriptor.bundlePath.empty()) {
                    logHostStage("resources.begin");
	                const std::string resourceDir = vst3ResourceDirectory(descriptor.bundlePath);
                    logHostStage(resourceDir.empty() ? "resources.none" : "resources.found");
                    const bool useResourceOverlay = shouldUseResourceOverlayForDescriptor(descriptor);
                    if (!resourceDir.empty() && useResourceOverlay) {
                        mirrorResourceOverlayIntoMainBundle(resourceDir, mirroredResourcePaths_);
                        logHostStage("resources.mirror.original.done");
                    } else if (!resourceDir.empty()) {
                        logHostStage("resources.mirror.original.skipped");
                    }
	                const std::string overlayDir = useResourceOverlay ? createResourceOverlayIfUseful(resourceDir) : std::string();
                    logHostStage(overlayDir.empty() ? "resources.overlay.skipped" : "resources.overlay.created");
	                resourceOverlayDirectory_ = overlayDir;
	                if (!overlayDir.empty()) {
                        logHostStage("resources.mirror.begin");
	                    mirrorResourceOverlayIntoMainBundle(overlayDir, mirroredResourcePaths_);
                        logHostStage("resources.mirror.done");
                        logHostStage("bundle.overlay.begin");
	                    bundlePathForLoad = createBundleOverlayIfUseful(descriptor.bundlePath, overlayDir, resourceBundleOverlayDirectory_);
	                    if (bundlePathForLoad.empty()) {
	                        bundlePathForLoad = descriptor.bundlePath;
	                    } else {
                        logHostStage("bundle.resources.overlay.ok");
                    }
                }
                std::string cwdDir = overlayDir.empty() ? resourceDir : overlayDir;
                if (std::getenv("NEURACOUST_VST3_EDITOR_USE_IZOTOPE_CORE_CWD") != nullptr) {
                    const std::string coreResources = izotopeCoreResourceDirectory(descriptor.bundlePath);
                    if (!coreResources.empty()) {
                        cwdDir = coreResources;
                    }
                }
                if (!cwdDir.empty() && chdir(cwdDir.c_str()) == 0) {
                    logHostStage(cwdDir == resourceDir ? "cwd.resources.ok" :
                                 (overlayDir.empty() ? "cwd.vendor.core.ok" : "cwd.resources.overlay.ok"));
                } else {
                    logHostStage("cwd.resources.skipped");
                }
            } else if (!descriptor.executablePath.empty()) {
                const std::string executableDir = directoryName(descriptor.executablePath);
                if (!executableDir.empty() && chdir(executableDir.c_str()) == 0) {
                    logHostStage("cwd.executable.ok");
                }
            }
		        const std::string modulePath = descriptor.executablePath.empty() ? descriptor.bundlePath : descriptor.executablePath;
		        if (modulePath.empty()) {
	            error = "VST3 module path is empty.";
	            return false;
	        }
		        using GetPluginFactoryFn = Steinberg::IPluginFactory* (*)();
	            GetPluginFactoryFn factoryFn = nullptr;
	            if (!descriptor.bundlePath.empty()) {
                    logHostStage("bundle.load.begin");
	                NSString* bundlePath = [NSString stringWithUTF8String:bundlePathForLoad.c_str()];
	                NSURL* bundleURL = [NSURL fileURLWithPath:bundlePath];
	                bundle_ = CFBundleCreate(kCFAllocatorDefault, (CFURLRef)bundleURL);
                if (bundle_ != nullptr) {
                    if (CFBundleLoadExecutable(bundle_)) {
                        using BundleEntryFn = bool (*)(CFBundleRef);
                        auto* bundleEntry = reinterpret_cast<BundleEntryFn>(
                            CFBundleGetFunctionPointerForName(bundle_, CFSTR("bundleEntry")));
                        bundleExit_ = reinterpret_cast<BundleExitFn>(
                            CFBundleGetFunctionPointerForName(bundle_, CFSTR("bundleExit")));
                        if (bundleEntry != nullptr) {
                            logHostStage("bundle.entry.begin");
                            if (!bundleEntry(bundle_)) {
                                logHostStage("bundle.entry.failed");
                                error = "Calling VST3 bundleEntry failed.";
                                CFBundleUnloadExecutable(bundle_);
                                CFRelease(bundle_);
                                bundle_ = nullptr;
                                return false;
                            }
                            bundleEntryCalled_ = true;
                            logHostStage("bundle.entry.ok");
                        } else {
                            logHostStage("bundle.entry.none");
                        }
                        factoryFn = reinterpret_cast<GetPluginFactoryFn>(CFBundleGetFunctionPointerForName(bundle_, CFSTR("GetPluginFactory")));
                    }
	                    if (factoryFn == nullptr) {
                        if (bundleEntryCalled_ && bundleExit_ != nullptr) {
                            bundleExit_();
                        }
                        bundleEntryCalled_ = false;
                        bundleExit_ = nullptr;
                        CFBundleUnloadExecutable(bundle_);
                        CFRelease(bundle_);
                        bundle_ = nullptr;
                    }
                }
                    logHostStage(factoryFn != nullptr ? "bundle.load.ok" : "bundle.load.fallback");
	            }
	            if (factoryFn == nullptr) {
                    logHostStage("dlopen.begin");
	                module_ = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (module_ == nullptr) {
                    const char* dlError = dlerror();
                    error = dlError != nullptr ? dlError : "Could not load VST3 module.";
                    return false;
	                }
	                factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(module_, "GetPluginFactory"));
                    logHostStage(factoryFn != nullptr ? "dlopen.ok" : "dlopen.no_factory");
	            }
	        if (factoryFn == nullptr) {
	            error = "GetPluginFactory symbol was not found.";
	            close();
            return false;
        }
            logHostStage("factory.begin");
	        Steinberg::IPluginFactory* factory = factoryFn();
        if (factory == nullptr) {
            error = "VST3 factory returned null.";
            close();
            return false;
        }
        if (std::getenv("NEURACOUST_VST3_EDITOR_LOG_CLASSES") != nullptr) {
            const int classCount = factory->countClasses();
            std::cerr << "HOST_CLASSES count=" << classCount << std::endl;
            for (int index = 0; index < classCount; ++index) {
                Steinberg::PClassInfo info;
                if (factory->getClassInfo(index, &info) == Steinberg::kResultOk) {
                    std::cerr << "HOST_CLASS index=" << index
                              << " cid=" << tuidToHex(info.cid)
                              << " category=" << info.category
                              << " name=" << info.name
                              << std::endl;
                }
            }
        }
            logHostStage("host.begin");
	        host_ = new EditorHostSupport();
        host_->trustRequestedResize = trustRequestedResize_;
        host_->parameterEditCallback = [](Steinberg::Vst::ParamID parameterId, Steinberg::Vst::ParamValue value) {
            std::cout << "PARAM " << static_cast<uint32_t>(parameterId) << " " << std::clamp(value, 0.0, 1.0) << std::endl;
        };
        host_->componentRestartCallback = [this](Steinberg::int32) {
            this->pollControllerParameterChanges(true);
        };
            logHostStage("component.begin");
	        if (!createComponent(factory, descriptor)) {
            logHostStage("component.failed");
            error = "No VST3 audio component class could be instantiated.";
            close();
            return false;
        }
            prepareEditorMeterProcessor();
            const bool looksLikeWavesProduct = lowercaseAscii(descriptor.bundlePath + " " +
                                                              descriptor.vendor + " " +
                                                              descriptor.brand + " " +
                                                              descriptor.componentClassName).find("waves") != std::string::npos &&
                lowercaseAscii(descriptor.componentClassName).find("immersive wrapper") != std::string::npos &&
                descriptor.name.find("Immersive Wrapper") == std::string::npos;
            if (looksLikeWavesProduct && std::getenv("NEURACOUST_DISABLE_WAVES_PRODUCT_STATE") == nullptr) {
                auto* defaultStateStream = new MemoryVst3Stream();
                Steinberg::tresult defaultStateResult = Steinberg::kInternalError;
                try {
                    defaultStateResult = component_->getState(defaultStateStream);
                } catch (...) {
                    defaultStateResult = Steinberg::kInternalError;
                }
                const auto patchedState = defaultStateResult == Steinberg::kResultOk
                    ? wavesStateForProduct(defaultStateStream->data(), descriptor.name, descriptor.componentClassName)
                    : std::vector<char>();
                defaultStateStream->release();
                if (!patchedState.empty()) {
                    auto* productStateStream = new MemoryVst3Stream();
                    productStateStream->setData(patchedState);
                    Steinberg::tresult setProductStateResult = Steinberg::kInternalError;
                    try {
                        setProductStateResult = component_->setState(productStateStream);
                    } catch (...) {
                        setProductStateResult = Steinberg::kInternalError;
                    }
                    productStateStream->release();
                    logHostStage(setProductStateResult == Steinberg::kResultOk
                        ? "waves.product.state.ok"
                        : "waves.product.state.failed");
                } else {
                    logHostStage("waves.product.state.unavailable");
                }
            }
            if (std::getenv("NEURACOUST_VST3_DUMP_COMPONENT_STATE") != nullptr) {
                auto* stateStream = new MemoryVst3Stream();
                Steinberg::tresult stateResult = Steinberg::kInternalError;
                try {
                    stateResult = component_->getState(stateStream);
                } catch (...) {
                    stateResult = Steinberg::kInternalError;
                }
                std::ostringstream stateStage;
                stateStage << "component.state.dump result=" << stateResult
                           << " bytes=" << stateStream->data().size()
                           << " preview=" << printableStatePreview(stateStream->data());
                logHostStage(stateStage.str());
                if (const char* dumpPath = std::getenv("NEURACOUST_VST3_DUMP_COMPONENT_STATE_PATH")) {
                    FILE* file = std::fopen(dumpPath, "wb");
                    if (file != nullptr) {
                        const auto& data = stateStream->data();
                        if (!data.empty()) {
                            std::fwrite(data.data(), 1, data.size(), file);
                        }
                        std::fclose(file);
                    }
                }
                stateStream->release();
            }
            logHostStage("controller.create.begin");
	        if (!createController(factory, error)) {
            logHostStage("controller.create.failed");
            close();
            return false;
        }
            if (controllerFromComponent_) {
                logHostStage("controller.initialize.skipped.component");
            } else {
                logHostStage("controller.initialize.begin");
                Steinberg::tresult controllerInitResult = Steinberg::kInternalError;
                try {
                    controllerInitResult = controller_->initialize(static_cast<Steinberg::Vst::IHostApplication*>(host_));
                } catch (const std::exception& exception) {
                    logHostStage("controller.initialize.exception");
                    error = std::string("VST3 edit controller threw during initialization: ") + exception.what();
                    close();
                    return false;
                } catch (...) {
                    logHostStage("controller.initialize.exception");
                    error = "VST3 edit controller threw during initialization.";
                    close();
                    return false;
                }
                if (controllerInitResult != Steinberg::kResultOk) {
                    logHostStage("controller.initialize.failed");
                    error = "VST3 edit controller initialization failed.";
                    close();
                    return false;
                }
                controllerInitialized_ = true;
            }
            transferComponentStateToController();
            if (shouldSkipComponentConnectionForEditor(descriptor)) {
                logHostStage("connection.skipped.vendor");
            } else {
                logHostStage("connection.begin");
                connectComponentAndController();
                logHostStage(componentControllerConnected_ ? "connection.ok" : "connection.skipped");
            }
            logHostStage("view.create.begin");
        try {
	        controller_->setComponentHandler(static_cast<Steinberg::Vst::IComponentHandler*>(host_));
	        view_ = controller_->createView(Steinberg::Vst::ViewType::kEditor);
        } catch (const std::exception& exception) {
            logHostStage("view.create.exception");
            error = std::string("VST3 editor threw while creating its view: ") + exception.what();
            close();
            return false;
        } catch (...) {
            logHostStage("view.create.exception");
            error = "VST3 editor threw while creating its view.";
            close();
            return false;
        }
        if (view_ == nullptr) {
            logHostStage("view.create.none");
            error = "VST3 plug-in did not provide an editor view.";
            close();
            return false;
        }
            logHostStage("view.platform.begin");
        Steinberg::tresult platformResult = Steinberg::kInternalError;
        try {
	        platformResult = view_->isPlatformTypeSupported(Steinberg::kPlatformTypeNSView);
        } catch (const std::exception& exception) {
            logHostStage("view.platform.exception");
            error = std::string("VST3 editor threw while checking NSView support: ") + exception.what();
            close();
            return false;
        } catch (...) {
            logHostStage("view.platform.exception");
            error = "VST3 editor threw while checking NSView support.";
            close();
            return false;
        }
	        if (platformResult != Steinberg::kResultTrue) {
            logHostStage("view.platform.unsupported");
            error = "VST3 editor does not support macOS NSView hosting.";
            close();
            return false;
        }
	        Steinberg::ViewRect sizeRect(0, 0, 720, 460);
	            logHostStage("view.size.begin");
        try {
		        if (view_->getSize(&sizeRect) != Steinberg::kResultTrue || sizeRect.getWidth() <= 0 || sizeRect.getHeight() <= 0) {
	            sizeRect = Steinberg::ViewRect(0, 0, 720, 460);
	        }
        } catch (...) {
            logHostStage("view.size.fallback");
            sizeRect = Steinberg::ViewRect(0, 0, 720, 460);
        }
        sizeRect = normalizedViewRect(sizeRect);
        flexibleInitialSizeCapActive_ = false;
        flexibleInitialSizeCapHeight_ = 0;
        if (const auto collapsedRect = initialFlexibleWavesCollapsedRect(descriptor, sizeRect)) {
            sizeRect = *collapsedRect;
            flexibleInitialSizeCapActive_ = true;
            flexibleInitialSizeCapHeight_ = sizeRect.getHeight();
            logHostStage("view.size.flexible.collapsed.initial");
        }
	        const bool viewCanResize = view_->canResize() == Steinberg::kResultTrue;
            viewCanResize_ = viewCanResize;
	        const CGFloat width = std::max<CGFloat>(minimumEditorWidth(), static_cast<CGFloat>(sizeRect.getWidth()));
	        const CGFloat height = std::max<CGFloat>(minimumEditorHeight(), static_cast<CGFloat>(sizeRect.getHeight()));
	        NSWindowStyleMask styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
	        if (viewCanResize) {
	            styleMask |= NSWindowStyleMaskResizable;
	        }
	        panel_ = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
	                                           styleMask:styleMask
	                                             backing:NSBackingStoreBuffered
	                                               defer:NO];
        panel_.title = title ?: @"VST3 Plug-in";
        panel_.releasedWhenClosed = NO;
        panel_.hidesOnDeactivate = NO;
        panel_.worksWhenModal = YES;
        panel_.level = NSFloatingWindowLevel;
        panel_.minSize = NSMakeSize(minimumEditorWidth(), minimumEditorHeight());
        panel_.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorMoveToActiveSpace;
        host_->panel = panel_;
        host_->viewResizedCallback = [this](const Steinberg::ViewRect& rect) {
            this->flexibleInitialSizeCapActive_ = false;
            const Steinberg::ViewRect normalized = this->normalizedFlexibleWavesRect(rect);
            this->recordViewSize(normalized);
            this->applyViewRectToWindow(normalized);
        };
        NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        panel_.contentView = content;
        [content release];
        if (meterOverlayEnabled_) {
            meterOverlay_ = [[NAMeterTelemetryOverlayView alloc] initWithFrame:NSMakeRect(10, height - 29, std::min<CGFloat>(360.0, width - 20.0), 24)];
            meterOverlay_.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
            meterOverlay_.hidden = YES;
            [panel_.contentView addSubview:meterOverlay_ positioned:NSWindowAbove relativeTo:nil];
        }
        view_->setFrame(static_cast<Steinberg::IPlugFrame*>(host_));
            logHostStage("view.attach.begin");
        Steinberg::tresult attachResult = Steinberg::kInternalError;
        try {
	        attachResult = view_->attached((__bridge void*)panel_.contentView, Steinberg::kPlatformTypeNSView);
        } catch (const std::exception& exception) {
            logHostStage("view.attach.exception");
            error = std::string("VST3 editor threw while attaching to NSView: ") + exception.what();
            close();
            return false;
        } catch (...) {
            logHostStage("view.attach.exception");
            error = "VST3 editor threw while attaching to NSView.";
            close();
            return false;
        }
	        if (attachResult != Steinberg::kResultTrue) {
            logHostStage("view.attach.failed");
            error = "VST3 editor attach failed.";
            close();
            return false;
        }
        if (meterOverlay_ != nil) {
            [meterOverlay_ removeFromSuperview];
            [panel_.contentView addSubview:meterOverlay_ positioned:NSWindowAbove relativeTo:nil];
        }
	        viewAttached_ = true;
            logHostStage("view.attach.ok");
        Steinberg::ViewRect attachedSizeRect = sizeRect;
        if (view_->getSize(&attachedSizeRect) == Steinberg::kResultTrue &&
	            attachedSizeRect.getWidth() > 0 &&
	            attachedSizeRect.getHeight() > 0) {
	            sizeRect = normalizedViewRect(attachedSizeRect);
            if (flexibleInitialSizeCapActive_ &&
                flexibleInitialSizeCapHeight_ > 0 &&
                sizeRect.getHeight() > flexibleInitialSizeCapHeight_) {
                sizeRect = Steinberg::ViewRect(0, 0, sizeRect.getWidth(), flexibleInitialSizeCapHeight_);
            }
            const CGFloat attachedWidth = std::max<CGFloat>(minimumEditorWidth(), static_cast<CGFloat>(sizeRect.getWidth()));
            const CGFloat attachedHeight = std::max<CGFloat>(minimumEditorHeight(), static_cast<CGFloat>(sizeRect.getHeight()));
            NSScreen* screen = panel_.screen ?: [NSScreen mainScreen];
            NSRect visibleFrame = screen != nil ? screen.visibleFrame : NSMakeRect(0, 0, attachedWidth, attachedHeight);
	            const CGFloat fittedWidth = viewCanResize ? std::min(attachedWidth, std::max<CGFloat>(minimumEditorWidth(), NSWidth(visibleFrame) - 64.0)) : attachedWidth;
	            const CGFloat fittedHeight = viewCanResize ? std::min(attachedHeight, std::max<CGFloat>(minimumEditorHeight(), NSHeight(visibleFrame) - 84.0)) : attachedHeight;
	            setContentSizeWithoutResizeCallback(NSMakeSize(fittedWidth, fittedHeight));
	            if (viewCanResize && (std::fabs(fittedWidth - attachedWidth) > 0.5 || std::fabs(fittedHeight - attachedHeight) > 0.5)) {
	                sizeRect = Steinberg::ViewRect(0,
	                                               0,
	                                               static_cast<Steinberg::int32>(std::round(fittedWidth)),
	                                               static_cast<Steinberg::int32>(std::round(fittedHeight)));
	                if (!trustRequestedResize_) {
	                    view_->checkSizeConstraint(&sizeRect);
	                }
                    sizeRect = normalizedViewRect(sizeRect);
	                const CGFloat constrainedWidth = std::max<CGFloat>(minimumEditorWidth(), static_cast<CGFloat>(sizeRect.getWidth()));
	                const CGFloat constrainedHeight = std::max<CGFloat>(minimumEditorHeight(), static_cast<CGFloat>(sizeRect.getHeight()));
	                setContentSizeWithoutResizeCallback(NSMakeSize(constrainedWidth, constrainedHeight));
	            }
	        }
	        view_->onSize(&sizeRect);
            recordViewSize(sizeRect);
        [panel_ center];
        [NSApp activateIgnoringOtherApps:YES];
        [panel_ makeKeyAndOrderFront:nil];
        [panel_ orderFrontRegardless];
        __block Vst3EditorSession* weakSession = this;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.15 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            weakSession->refreshAttachedViewSizeFromPlugin();
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.50 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            weakSession->refreshAttachedViewSizeFromPlugin();
        });
        installFlexibleWavesResizeMonitorIfNeeded();
        installTransportKeyMonitor();
        closeObserver_ = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                                           object:panel_
                                                                            queue:[NSOperationQueue mainQueue]
                                                                       usingBlock:^(NSNotification*) {
                                                                           [NSApp terminate:nil];
                                                                       }];
        resizeObserver_ = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResizeNotification
                                                                            object:panel_
                                                                             queue:[NSOperationQueue mainQueue]
                                                                        usingBlock:^(NSNotification*) {
                                                                            this->handleWindowResize();
                                                                        }];
            logHostStage("open.ok");
	        return true;
	    }

    void close() {
        // Stop the real-time meter pump first: it holds a raw `this` and fires on the main queue,
        // so it must never fire once the session's meterProcessor_/controller_ are torn down below.
        // close() runs on the main thread and the timer fires on the main queue, so cancel here can
        // not race a live handler.
        audioBridgeStop_.store(true);
        if (bridgeMeterTimer_ != nullptr) {
            dispatch_source_cancel(bridgeMeterTimer_);
            dispatch_release(bridgeMeterTimer_);
            bridgeMeterTimer_ = nullptr;
        }
        if (transportKeyMonitor_ != nil) {
            [NSEvent removeMonitor:transportKeyMonitor_];
            transportKeyMonitor_ = nil;
        }
        if (flexibleResizeEventMonitor_ != nil) {
            [NSEvent removeMonitor:flexibleResizeEventMonitor_];
            flexibleResizeEventMonitor_ = nil;
        }
        if (resizeObserver_ != nil) {
            [[NSNotificationCenter defaultCenter] removeObserver:resizeObserver_];
            resizeObserver_ = nil;
        }
        if (closeObserver_ != nil) {
            [[NSNotificationCenter defaultCenter] removeObserver:closeObserver_];
            closeObserver_ = nil;
        }
        if (view_ != nullptr) {
            view_->setFrame(nullptr);
            if (viewAttached_) {
                view_->removed();
                viewAttached_ = false;
            }
            view_->release();
            view_ = nullptr;
        }
        if (controller_ != nullptr) {
                disconnectComponentAndController();
	            if (controllerInitialized_) {
	                controller_->terminate();
	                controllerInitialized_ = false;
            }
            controller_->release();
            controller_ = nullptr;
        }
        if (meterProcessor_ != nullptr) {
            try {
                meterProcessor_->setProcessing(false);
            } catch (...) {
            }
            meterProcessor_->release();
            meterProcessor_ = nullptr;
        }
        if (component_ != nullptr) {
            try {
                component_->setActive(false);
            } catch (...) {
            }
            if (componentInitialized_) {
                component_->terminate();
                componentInitialized_ = false;
            }
            component_->release();
            component_ = nullptr;
        }
        if (host_ != nullptr) {
            host_->panel = nil;
            host_->viewResizedCallback = nullptr;
            host_->release();
            host_ = nullptr;
        }
            if (meterOverlay_ != nil) {
                [meterOverlay_ removeFromSuperview];
                [meterOverlay_ release];
                meterOverlay_ = nil;
            }
        if (panel_ != nil) {
            [panel_ orderOut:nil];
            [panel_ release];
            panel_ = nil;
        }
            if (bundle_ != nullptr) {
                if (bundleEntryCalled_ && bundleExit_ != nullptr) {
                    bundleExit_();
                }
                bundleEntryCalled_ = false;
                bundleExit_ = nullptr;
                CFBundleUnloadExecutable(bundle_);
                CFRelease(bundle_);
                bundle_ = nullptr;
            }
	        if (module_ != nullptr) {
	            dlclose(module_);
	            module_ = nullptr;
	        }
            const bool keepResourceOverlays = std::getenv("NEURACOUST_KEEP_VST3_OVERLAY") != nullptr;
            if (!keepResourceOverlays && !resourceOverlayDirectory_.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(resourceOverlayDirectory_, ec);
                resourceOverlayDirectory_.clear();
            }
            if (!keepResourceOverlays && !resourceBundleOverlayDirectory_.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(resourceBundleOverlayDirectory_, ec);
                resourceBundleOverlayDirectory_.clear();
            }
            for (auto it = mirroredResourcePaths_.rbegin(); it != mirroredResourcePaths_.rend(); ++it) {
                std::error_code ec;
                std::filesystem::remove(*it, ec);
            }
            mirroredResourcePaths_.clear();
    }

    private:
        void installTransportKeyMonitor() {
            if (transportKeyMonitor_ != nil || panel_ == nil) {
                return;
            }
            __block NSPanel* targetPanel = panel_;
            transportKeyMonitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                                         handler:^NSEvent* (NSEvent* event) {
                                                                             if (event.window != targetPanel || !isPlainSpaceKeyEvent(event)) {
                                                                                 return event;
                                                                             }
                                                                             if (responderAcceptsTextInput(targetPanel.firstResponder)) {
                                                                                 return event;
                                                                             }
                                                                             postPluginEditorTransportToggle();
                                                                             return nil;
                                                                         }];
        }

        void transferComponentStateToController() {
            if (!allowComponentStateTransfer_) {
                logHostStage("component.state.skipped.safe_default");
                return;
            }
            if (component_ == nullptr || controller_ == nullptr || controllerFromComponent_) {
                logHostStage("component.state.skipped");
                return;
            }
            auto* stream = new MemoryVst3Stream();
            Steinberg::tresult getStateResult = Steinberg::kInternalError;
            try {
                getStateResult = component_->getState(stream);
            } catch (...) {
                getStateResult = Steinberg::kInternalError;
            }
            if (getStateResult != Steinberg::kResultOk || stream->empty()) {
                stream->release();
                logHostStage("component.state.none");
                return;
            }
            stream->rewind();
            Steinberg::tresult setStateResult = Steinberg::kInternalError;
            try {
                setStateResult = controller_->setComponentState(stream);
            } catch (...) {
                setStateResult = Steinberg::kInternalError;
            }
            stream->release();
            logHostStage(setStateResult == Steinberg::kResultOk ? "component.state.ok" : "component.state.failed");
        }

        bool pointLooksLikeFlexibleWavesResizeButton(NSPoint point) const {
            if (panel_ == nil || panel_.contentView == nil || flexibleInitialSizeCapHeight_ <= 0) {
                return false;
            }
            const NSRect bounds = panel_.contentView.bounds;
            if (!NSPointInRect(point, bounds)) {
                return false;
            }
            const CGFloat bottomBand = std::min<CGFloat>(92.0, std::max<CGFloat>(46.0, NSHeight(bounds) * 0.24));
            const BOOL inBottomBand = point.y <= bottomBand;
            const BOOL nearLeftResizeControl = point.x <= 150.0;
            const BOOL nearRightResizeControl = point.x >= NSWidth(bounds) - 170.0;
            return inBottomBand && (nearLeftResizeControl || nearRightResizeControl);
        }

        void scheduleFlexibleWavesSizeRefreshAfterUserClick() {
            flexibleInitialSizeCapActive_ = false;
            if (flexibleWavesRs124_) {
                const Steinberg::int32 currentWidth = hasCurrentSize_ && currentSizeRect_.getWidth() > 0
                    ? currentSizeRect_.getWidth()
                    : static_cast<Steinberg::int32>(std::round(panel_ != nil ? panel_.contentView.bounds.size.width : 840.0));
                const CGFloat currentHeight = panel_ != nil ? panel_.contentView.bounds.size.height : static_cast<CGFloat>(flexibleInitialSizeCapHeight_);
                const Steinberg::int32 nextHeight = currentHeight <= 430.0 ? 540 : 365;
                Steinberg::ViewRect forcedRect(0, 0, std::max<Steinberg::int32>(1, currentWidth), nextHeight);
                applyViewRectToWindow(forcedRect);
                recordViewSize(forcedRect);
            }
            __block Vst3EditorSession* weakSession = this;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.05 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.22 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
        }

        void schedulePluginViewSizeRefreshAfterUserInteraction() {
            flexibleInitialSizeCapActive_ = false;
            __block Vst3EditorSession* weakSession = this;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.04 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.18 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.55 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.10 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                weakSession->refreshAttachedViewSizeFromPlugin();
            });
        }

        void installFlexibleWavesResizeMonitorIfNeeded() {
            if (flexibleResizeEventMonitor_ != nil || panel_ == nil || !wavesEditor_) {
                return;
            }
            __block Vst3EditorSession* weakSession = this;
            flexibleResizeEventMonitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseUp
                                                                                handler:^NSEvent* (NSEvent* event) {
                if (weakSession == nullptr ||
                    weakSession->panel_ == nil ||
                    weakSession->panel_.contentView == nil) {
                    return event;
                }
                if (event.window == weakSession->panel_) {
                    const NSPoint point = [weakSession->panel_.contentView convertPoint:event.locationInWindow fromView:nil];
                    if (weakSession->pointLooksLikeFlexibleWavesResizeButton(point)) {
                        weakSession->scheduleFlexibleWavesSizeRefreshAfterUserClick();
                    } else {
                        weakSession->schedulePluginViewSizeRefreshAfterUserInteraction();
                    }
                } else if (weakSession->panel_.isKeyWindow || weakSession->panel_.isMainWindow || weakSession->panel_.visible) {
                    // Waves scale and skin menus may commit from a transient menu window, after the editor panel has handled the click.
                    weakSession->schedulePluginViewSizeRefreshAfterUserInteraction();
                }
                return event;
            }];
        }

        void setContentSizeWithoutResizeCallback(NSSize size) {
            if (panel_ == nil) {
                return;
            }
            suppressWindowResizeCallback_ = true;
            [panel_ setContentSize:size];
            panel_.contentView.frame = NSMakeRect(0, 0, size.width, size.height);
            suppressWindowResizeCallback_ = false;
        }

        CGFloat minimumEditorWidth() const {
            return wavesEditor_ ? 160.0 : 320.0;
        }

        CGFloat minimumEditorHeight() const {
            return wavesEditor_ ? 120.0 : 180.0;
        }

        void recordViewSize(const Steinberg::ViewRect& rect) {
            currentSizeRect_ = normalizedViewRect(rect);
            hasCurrentSize_ = rect.getWidth() > 0 && rect.getHeight() > 0;
        }

        std::optional<Steinberg::ViewRect> largestAttachedSubviewRect() const {
            if (!wavesEditor_ || panel_ == nil || panel_.contentView == nil) {
                return std::nullopt;
            }
            NSView* bestView = nil;
            CGFloat bestArea = 0.0;
            for (NSView* subview in panel_.contentView.subviews) {
                if (subview == meterOverlay_ || subview.hidden) {
                    continue;
                }
                const NSRect frame = subview.frame;
                if (NSWidth(frame) < minimumEditorWidth() || NSHeight(frame) < minimumEditorHeight()) {
                    continue;
                }
                const CGFloat area = NSWidth(frame) * NSHeight(frame);
                if (area > bestArea) {
                    bestArea = area;
                    bestView = subview;
                }
            }
            if (bestView == nil) {
                return std::nullopt;
            }
            const NSRect frame = bestView.frame;
            return Steinberg::ViewRect(0,
                                       0,
                                       static_cast<Steinberg::int32>(std::round(NSWidth(frame))),
                                       static_cast<Steinberg::int32>(std::round(NSHeight(frame))));
        }

        Steinberg::ViewRect chooseWavesViewRectCandidate(const Steinberg::ViewRect& pluginRect) const {
            const auto subviewRect = largestAttachedSubviewRect();
            bool usedSubview = false;
            const Steinberg::ViewRect candidate = chooseWavesViewRectCandidateForSubview(
                pluginRect,
                subviewRect,
                static_cast<Steinberg::int32>(std::round(minimumEditorWidth())),
                static_cast<Steinberg::int32>(std::round(minimumEditorHeight())),
                &usedSubview);
            if (usedSubview) {
                const Steinberg::ViewRect plugin = normalizedViewRect(pluginRect);
                logHostStage("view.size.waves.subview.candidate plugin=" +
                             std::to_string(plugin.getWidth()) + "x" +
                             std::to_string(plugin.getHeight()) + " subview=" +
                             std::to_string(candidate.getWidth()) + "x" +
                             std::to_string(candidate.getHeight()));
            }
            return candidate;
        }

        Steinberg::ViewRect normalizedFlexibleWavesRect(const Steinberg::ViewRect& rect) const {
            return normalizedFlexibleWavesRectForState(rect,
                                                       flexibleWavesRs124_,
                                                       flexibleInitialSizeCapActive_,
                                                       flexibleInitialSizeCapHeight_);
        }

        void applyViewRectToWindow(const Steinberg::ViewRect& rect) {
            if (panel_ == nil || panel_.contentView == nil) {
                return;
            }
            const Steinberg::ViewRect normalized = normalizedFlexibleWavesRect(rect);
            const CGFloat nextWidth = std::max<CGFloat>(minimumEditorWidth(), static_cast<CGFloat>(normalized.getWidth()));
            const CGFloat nextHeight = std::max<CGFloat>(minimumEditorHeight(), static_cast<CGFloat>(normalized.getHeight()));
            const NSSize current = panel_.contentView.bounds.size;
            if (std::fabs(nextWidth - current.width) <= 0.5 && std::fabs(nextHeight - current.height) <= 0.5) {
                panel_.contentView.frame = NSMakeRect(0, 0, current.width, current.height);
                return;
            }
            setContentSizeWithoutResizeCallback(NSMakeSize(nextWidth, nextHeight));
            if (view_ != nullptr && viewAttached_) {
                Steinberg::ViewRect applyRect = normalized;
                view_->onSize(&applyRect);
            }
        }

        void refreshAttachedViewSizeFromPlugin() {
            if (view_ == nullptr || !viewAttached_ || panel_ == nil || panel_.contentView == nil) {
                return;
            }
            Steinberg::ViewRect pluginRect = hasCurrentSize_
                ? currentSizeRect_
                : Steinberg::ViewRect(0, 0, 720, 460);
            if (view_->getSize(&pluginRect) != Steinberg::kResultTrue ||
                pluginRect.getWidth() <= 0 ||
                pluginRect.getHeight() <= 0) {
                pluginRect = hasCurrentSize_ ? currentSizeRect_ : Steinberg::ViewRect(0, 0, 720, 460);
            }
            if (viewCanResize_ && !trustRequestedResize_) {
                view_->checkSizeConstraint(&pluginRect);
            }
            if (wavesEditor_) {
                pluginRect = chooseWavesViewRectCandidate(pluginRect);
            }
            pluginRect = normalizedFlexibleWavesRect(pluginRect);
            applyViewRectToWindow(pluginRect);
            view_->onSize(&pluginRect);
            recordViewSize(pluginRect);
        }

    public:
        void setExternalMeterLevels(double inputLeft, double inputRight, double outputLeft, double outputRight) {
            latestMeterInputLeft_ = std::clamp(inputLeft, 0.0, 1.0);
            latestMeterInputRight_ = std::clamp(inputRight, 0.0, 1.0);
            latestMeterOutputLeft_ = std::clamp(outputLeft, 0.0, 1.0);
            latestMeterOutputRight_ = std::clamp(outputRight, 0.0, 1.0);
            lastMeterUpdateSeconds_ = [NSDate timeIntervalSinceReferenceDate];
            if (meterOverlay_ == nil) {
                return;
            }
            [meterOverlay_ setInputLeft:inputLeft inputRight:inputRight outputLeft:outputLeft outputRight:outputRight];
        }

        // Real-time-paced bridge metering. Runs on a fast (~2 ms) main-thread timer, NOT the 60 Hz
        // meter tick: FabFilter's limiter scopes place their waveform by the real wall-clock gap
        // between process() calls, so draining a whole 60 Hz burst at once stacked every block at
        // one x and left the rest of the frame blank — a regular comb. Firing ~every audio-block
        // period feeds one block at a time, so the scope advances smoothly like real playback.
        // process() stays on the main thread so the plug-in's IConnectionPoint meter/analyzer
        // messages still reach and repaint the GUI.
        void pumpBridgeMeterBlocks() {
            if (!audioBridgeActive_) {
                return;
            }
            // Feed the plug-in EXACTLY ONE block per fire. FabFilter's limiter scope fills its
            // waveform history by the wall-clock gap between successive process() calls, so if a
            // single fire issued several process() calls back-to-back the plug-in stamped them all
            // at the same instant (overlap) and then left a hole until the next fire — the comb.
            // One call per fast (~2 ms) fire makes each process() land at its own wall-clock tick,
            // exactly like a live host's per-buffer callback. An empty fire simply skips (the scope
            // pauses a tick, never holes); the ~2 ms fire outruns the ~2.2 ms average block arrival
            // so the queue never backs up for long. A deep backlog (a sub-block burst) is caught up
            // one-per-fire over the next few ms — still one call per tick, so still no overlap.
            BridgeCapturedBlock block;
            bool haveBlock = false;
            {
                std::lock_guard<std::mutex> lock(bridgeCaptureMutex_);
                if (!bridgeCaptureQueue_.empty()) {
                    block = std::move(bridgeCaptureQueue_.front());
                    bridgeCaptureQueue_.pop_front();
                    haveBlock = true;
                }
            }
            if (haveBlock) {
                bridgeEmptyDrainTicks_ = 0;
                if (block.frameCount > 0 &&
                    block.input.size() >= static_cast<size_t>(block.frameCount) * 2u) {
                    std::vector<float> scratch(static_cast<size_t>(block.frameCount) * 2u, 0.0f);
                    processBridgeBlock(block.input.data(), block.frameCount, block.params, scratch.data());
                }
                return;
            }
            // Nothing queued. A momentary gap mid-playback just skips (the scope pauses); only once
            // genuinely dry for a while (transport stopped) do we push decay-silence so the meters
            // fall to zero — feeding silence between real blocks would comb the scope.
            ++bridgeEmptyDrainTicks_;
            if (bridgeEmptyDrainTicks_ >= 64) {   // ~130 ms at ~2 ms/fire → stopped, not a gap
                const int fc = std::min(256, audioBridgeMaxBlock_);
                std::vector<float> silence(static_cast<size_t>(fc) * 2u, 0.0f);
                std::vector<float> silenceScratch(static_cast<size_t>(fc) * 2u, 0.0f);
                processBridgeBlock(silence.data(), fc, {}, silenceScratch.data());
            }
        }

        void tickEditorMeterProcessor() {
            if (audioBridgeActive_) {
                return;   // bridge metering runs on its own real-time-paced timer (pumpBridgeMeterBlocks)
            }
            const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
            const bool recentlyUpdated = lastMeterUpdateSeconds_ > 0.0 && now - lastMeterUpdateSeconds_ < 0.50;
            const double inputLeft = recentlyUpdated ? latestMeterInputLeft_ : 0.0;
            const double inputRight = recentlyUpdated ? latestMeterInputRight_ : 0.0;
            const double elapsedSeconds = lastMeterProcessSeconds_ > 0.0
                ? std::max(0.0, static_cast<double>(now - lastMeterProcessSeconds_))
                : (1.0 / 30.0);
            lastMeterProcessSeconds_ = now;
            constexpr double kSyntheticMeterSampleRate = 48000.0;
            constexpr int kSyntheticMeterBlockFrames = 256;
            const int blockCount = std::max(1, std::min(16, static_cast<int>(
                std::ceil((elapsedSeconds * kSyntheticMeterSampleRate) /
                          static_cast<double>(kSyntheticMeterBlockFrames)))));
            for (int block = 0; block < blockCount; ++block) {
                processEditorMeterAudio(inputLeft, inputRight);
            }
        }

        void configureAudioBridge(const std::string& shmName, int maxBlock, double sampleRate, bool observer) {
            audioBridgeShmName_ = shmName;
            audioBridgeMaxBlock_ = std::max(1, maxBlock);
            audioBridgeSampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
            audioBridgeActive_ = !shmName.empty();
            audioBridgeObserver_ = observer;
        }

        bool audioBridgeActive() const { return audioBridgeActive_; }

        // Serve the engine's realtime audio for this insert using the SAME plugin
        // instance the editor GUI is attached to, so the plugin's own meters and
        // processing reflect the real track audio.
        void startAudioBridge() {
            if (!audioBridgeActive_ || meterProcessor_ == nullptr || audioBridgeThread_.joinable()) {
                return;
            }
            bridgeInLeft_.assign(static_cast<size_t>(audioBridgeMaxBlock_), 0.0f);
            bridgeInRight_.assign(static_cast<size_t>(audioBridgeMaxBlock_), 0.0f);
            bridgeOutLeft_.assign(static_cast<size_t>(audioBridgeMaxBlock_), 0.0f);
            bridgeOutRight_.assign(static_cast<size_t>(audioBridgeMaxBlock_), 0.0f);
            const std::string shm = audioBridgeShmName_;
            const int maxBlock = audioBridgeMaxBlock_;
            if (audioBridgeObserver_) {
                audioBridgeThread_ = std::thread([this, shm, maxBlock]() { runObserverLoop(shm, maxBlock); });
                audioBridgeThread_.detach();
                // Drain the captured blocks on a fast main-thread timer (~2 ms, an audio-block
                // period) so process() is paced like real playback and the limiter scopes don't
                // comb. Main queue → the plug-in's meter/analyzer GUI messages still land on-main.
                bridgeMeterTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                           dispatch_get_main_queue());
                if (bridgeMeterTimer_ != nullptr) {
                    dispatch_source_set_timer(bridgeMeterTimer_, DISPATCH_TIME_NOW,
                                              2ull * NSEC_PER_MSEC, 1ull * NSEC_PER_MSEC);
                    __block Vst3EditorSession* self = this;
                    dispatch_source_set_event_handler(bridgeMeterTimer_, ^{ self->pumpBridgeMeterBlocks(); });
                    dispatch_resume(bridgeMeterTimer_);
                }
                return;
            }
            audioBridgeThread_ = std::thread([this, shm, maxBlock]() {
                neuracoust::daw::Vst3BridgeServer server;
                if (!server.attach(shm, maxBlock)) {
                    return;
                }
                server.announceReady(true, 0u, 0u, descriptor_.name,
                                     "VST3 hosted in editor (GUI + audio).");
                std::vector<float> outputBlock(static_cast<size_t>(maxBlock) * 2u, 0.0f);
                int frameCount = 0;
                std::vector<neuracoust::daw::Vst3BridgeParam> inParams;
                while (server.waitRequest(frameCount, inParams)) {
                    const bool ok = processBridgeBlock(server.input(), frameCount, inParams, outputBlock.data());
                    server.postResponse(ok, ok ? outputBlock.data() : nullptr, 0u, 0u, {});
                }
                server.detach();
            });
            audioBridgeThread_.detach();
        }

        // Read-only observer: watch the engine's realtime bridge for this insert
        // and run the SAME input through this editor's plugin instance purely to
        // animate the plugin's own meters/graphics. Never writes to the shared
        // memory or participates in the handshake, so it cannot affect the audio.
        void runObserverLoop(const std::string& shmName, int maxBlock) {
            // Metering-only: this thread re-runs the plug-in at audio rate purely to
            // animate the plug-in's own meters/graphics. It must NEVER compete with
            // the DAW's realtime audio thread or the audio-processing worker for CPU
            // cores — otherwise several open editors saturate the machine and the
            // audio callback is delivered late (high wake jitter even though the
            // in-callback DSP load stays low). Run it below normal priority: under
            // load the meters may lag a little, which is harmless, but audio stays
            // glitch-free.
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
            const auto layout = neuracoust::daw::vst3BridgeComputeLayout(maxBlock);
            void* mapping = nullptr;
            int fd = -1;
            std::vector<neuracoust::daw::Vst3BridgeParam> params;
            uint32_t lastRequestSeq = 0;
            int staleIterations = 0;
            bool advancedSinceAttach = false;
            constexpr int kSilenceThreshold = 50;  // ~100 ms of no new block → stopped → silence
            constexpr int kReattachLimit = 250;    // ~500 ms stale → re-open to catch a recreated shm
            auto detach = [&]() {
                if (mapping != nullptr) {
                    munmap(mapping, layout.totalSize);
                    mapping = nullptr;
                }
                if (fd >= 0) {
                    ::close(fd);
                    fd = -1;
                }
            };
            while (!audioBridgeStop_.load()) {
                // (Re)attach: the engine creates the bridge lazily on first playback
                // and tears it down / recreates it when the insert switches between
                // Native and Internal DSP, so we must be able to pick up a new shm.
                if (mapping == nullptr) {
                    fd = shm_open(shmName.c_str(), O_RDONLY, 0600);
                    if (fd >= 0) {
                        void* m = mmap(nullptr, layout.totalSize, PROT_READ, MAP_SHARED, fd, 0);
                        if (m != MAP_FAILED) {
                            const auto* cb = reinterpret_cast<const neuracoust::daw::Vst3BridgeControlBlock*>(m);
                            if (cb->magic == neuracoust::daw::kVst3BridgeMagic) {
                                mapping = m;
                                lastRequestSeq = cb->requestSeq;
                                staleIterations = 0;
                                advancedSinceAttach = false;
                            } else {
                                munmap(m, layout.totalSize);
                            }
                        }
                        if (mapping == nullptr) {
                            ::close(fd);
                            fd = -1;
                        }
                    }
                }

                bool freshBlock = false;
                if (mapping != nullptr) {
                    const auto* cb = reinterpret_cast<const neuracoust::daw::Vst3BridgeControlBlock*>(mapping);
                    if (cb->requestSeq != lastRequestSeq) {
                        lastRequestSeq = cb->requestSeq;
                        staleIterations = 0;
                        advancedSinceAttach = true;
                        freshBlock = true;
                    } else {
                        ++staleIterations;
                    }
                    const int frameCount = std::max(0, std::min(cb->frameCount, maxBlock));
                    // Feed each engine block through the plug-in EXACTLY ONCE, in order,
                    // and only when it is genuinely new. Re-feeding the same block (or
                    // skipping) breaks sample continuity, which a spectrum analyzer shows
                    // as a smeared tone. Polling faster than the block rate lets us catch
                    // every block without repeats.
                    if (freshBlock && advancedSinceAttach && frameCount > 0) {
                        const int paramCount = std::max(0, std::min(cb->numInParams, neuracoust::daw::kVst3BridgeMaxParams));
                        params.clear();
                        const auto* inParams = neuracoust::daw::vst3BridgeParamRegion(const_cast<void*>(mapping), layout.inParamsOffset);
                        for (int i = 0; i < paramCount; ++i) {
                            params.push_back(inParams[i]);
                        }
                        const float* input = neuracoust::daw::vst3BridgeAudioRegion(const_cast<void*>(mapping), layout.inputOffset);
                        // Hand the block to the main thread — DON'T run process() here. The
                        // plug-in's meter/analyzer messages fire during process() over its
                        // component↔controller connection; on this background thread they never
                        // reach the GUI. Queued in order so the analyzer stream stays continuous.
                        BridgeCapturedBlock captured;
                        captured.input.assign(input, input + static_cast<size_t>(frameCount) * 2u);
                        captured.frameCount = frameCount;
                        captured.params = params;
                        {
                            std::lock_guard<std::mutex> lock(bridgeCaptureMutex_);
                            if (bridgeCaptureQueue_.size() >= kBridgeCaptureQueueCap) {
                                bridgeCaptureQueue_.pop_front();   // UI stalled — drop oldest
                            }
                            bridgeCaptureQueue_.push_back(std::move(captured));
                        }
                    }
                    if (staleIterations > kReattachLimit) {
                        // Long-idle: re-open in case the shm was torn down and a new one
                        // recreated (e.g. an Internal DSP ↔ Native switch).
                        detach();
                    }
                }
                // Idle silence is fed by the main-thread tick when the queue runs dry, so the
                // meters decay on the same thread the plug-in expects — nothing to do here.
                usleep(2000);
            }
            detach();
        }

	        void setHostParameterValue(uint32_t parameterId, double normalizedValue) {
	            if (controller_ == nullptr) {
	                return;
	            }
	            const Steinberg::Vst::ParamValue value = std::clamp(normalizedValue, 0.0, 1.0);
            try {
                controller_->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(parameterId), value);
            } catch (...) {
            }
	            lastPolledParameterValues_[parameterId] = value;
	        }

	        void forwardParameterValueToDaw(uint32_t parameterId, double normalizedValue) {
	            const double value = std::clamp(normalizedValue, 0.0, 1.0);
	            const auto found = lastPolledParameterValues_.find(parameterId);
	            const std::optional<double> previousValue = found == lastPolledParameterValues_.end()
	                ? std::nullopt
	                : std::optional<double>(found->second);
	            if (!shouldForwardPolledParameterValue(previousValue, value)) {
	                return;
	            }
	            lastPolledParameterValues_[parameterId] = value;
	            std::cout << "PARAM " << parameterId << " " << value << std::endl;
	        }

        void initializeParameterPolling() {
            lastPolledParameterValues_.clear();
            lastParameterSnapshotSeconds_ = [NSDate timeIntervalSinceReferenceDate];
            if (controller_ == nullptr) {
                return;
            }
            const int32_t count = std::max<int32_t>(0, controller_->getParameterCount());
            for (int32_t index = 0; index < count; ++index) {
                Steinberg::Vst::ParameterInfo info {};
                try {
                    if (controller_->getParameterInfo(index, info) != Steinberg::kResultOk) {
                        continue;
                    }
                    if ((info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0) {
                        continue;
                    }
                    if (!wavesEditor_ &&
                        (info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) == 0) {
                        continue;
                    }
                    lastPolledParameterValues_[static_cast<uint32_t>(info.id)] = controller_->getParamNormalized(info.id);
                } catch (...) {
                }
            }
        }

        void pollControllerParameterChanges(bool forceSnapshot = false) {
            if (controller_ == nullptr) {
                return;
            }
            const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
            if (forceSnapshot || now - lastParameterSnapshotSeconds_ >= 0.20) {
                lastParameterSnapshotSeconds_ = now;
            }
            const int32_t count = std::max<int32_t>(0, controller_->getParameterCount());
            for (int32_t index = 0; index < count; ++index) {
                Steinberg::Vst::ParameterInfo info {};
                try {
                    if (controller_->getParameterInfo(index, info) != Steinberg::kResultOk) {
                        continue;
                    }
                    if ((info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0) {
                        continue;
                    }
                    if (!wavesEditor_ &&
                        (info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) == 0) {
                        continue;
                    }
	                    forwardParameterValueToDaw(static_cast<uint32_t>(info.id),
	                                               controller_->getParamNormalized(info.id));
	                } catch (...) {
	                }
	            }
	        }

        void handleWindowResize() {
            if (suppressWindowResizeCallback_ || view_ == nullptr || !viewAttached_ || panel_ == nil || panel_.contentView == nil) {
                return;
            }
            NSSize contentSize = panel_.contentView.bounds.size;
            Steinberg::ViewRect requested(0,
                                          0,
                                          static_cast<Steinberg::int32>(std::round(contentSize.width)),
                                          static_cast<Steinberg::int32>(std::round(contentSize.height)));
            if (!viewCanResize_ && hasCurrentSize_) {
                requested = currentSizeRect_;
            } else if (viewCanResize_ && !trustRequestedResize_) {
                view_->checkSizeConstraint(&requested);
            }
            requested = normalizedFlexibleWavesRect(requested);

            const CGFloat width = std::max<CGFloat>(minimumEditorWidth(), static_cast<CGFloat>(requested.getWidth()));
            const CGFloat height = std::max<CGFloat>(minimumEditorHeight(), static_cast<CGFloat>(requested.getHeight()));
            if (std::fabs(width - contentSize.width) > 0.5 || std::fabs(height - contentSize.height) > 0.5) {
                setContentSizeWithoutResizeCallback(NSMakeSize(width, height));
            }
            view_->onSize(&requested);
            recordViewSize(requested);
            if (wavesEditor_) {
                schedulePluginViewSizeRefreshAfterUserInteraction();
            }
        }

	    private:
	        void prepareEditorMeterProcessor() {
	            // In audio-bridge mode the processor must be prepared even when the
	            // synthetic meter probe is disabled, because it processes the real
	            // track audio for this insert.
	            if ((suppressSyntheticEditorAudio_ && !audioBridgeActive_) ||
	                component_ == nullptr ||
	                meterProcessor_ != nullptr) {
	                if (suppressSyntheticEditorAudio_ && !audioBridgeActive_) {
	                    logHostStage("editor.meter.processor.skipped.synthetic_audio_disabled");
	                }
	                return;
	            }
            void* processorObject = nullptr;
            if (component_->queryInterface(Steinberg::Vst::IAudioProcessor::iid, &processorObject) != Steinberg::kResultOk ||
                processorObject == nullptr) {
                return;
            }
            meterProcessor_ = static_cast<Steinberg::Vst::IAudioProcessor*>(processorObject);
            if (meterProcessor_->canProcessSampleSize(Steinberg::Vst::kSample32) != Steinberg::kResultOk) {
                meterProcessor_->release();
                meterProcessor_ = nullptr;
                return;
            }
            meterInputBusCount_ = std::max<int>(0, component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput));
            meterOutputBusCount_ = std::max<int>(0, component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput));
            if (meterInputBusCount_ > 0) {
                component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
            }
            if (meterOutputBusCount_ > 0) {
                component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
            }
            if (!setPreferredEditorMeterBusArrangement(meterProcessor_,
                                                       meterInputBusCount_,
                                                       meterOutputBusCount_,
                                                       meterInputChannelCount_,
                                                       meterOutputChannelCount_)) {
                Steinberg::Vst::BusInfo inputBusInfo {};
                Steinberg::Vst::BusInfo outputBusInfo {};
                if (meterInputBusCount_ > 0 &&
                    component_->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, inputBusInfo) == Steinberg::kResultOk) {
                    meterInputChannelCount_ = std::max(1, std::min(2, static_cast<int>(inputBusInfo.channelCount)));
                }
                if (meterOutputBusCount_ > 0 &&
                    component_->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, outputBusInfo) == Steinberg::kResultOk) {
                    meterOutputChannelCount_ = std::max(1, std::min(2, static_cast<int>(outputBusInfo.channelCount)));
                }
            }
            Steinberg::Vst::ProcessSetup setup {};
            setup.processMode = Steinberg::Vst::kRealtime;
            setup.symbolicSampleSize = Steinberg::Vst::kSample32;
            setup.maxSamplesPerBlock = audioBridgeActive_ ? audioBridgeMaxBlock_ : 256;
            setup.sampleRate = audioBridgeActive_ ? audioBridgeSampleRate_ : 48000.0;
            if (meterProcessor_->setupProcessing(setup) != Steinberg::kResultOk ||
                component_->setActive(true) != Steinberg::kResultOk ||
                meterProcessor_->setProcessing(true) != Steinberg::kResultOk) {
                meterProcessor_->setProcessing(false);
                meterProcessor_->release();
                meterProcessor_ = nullptr;
                return;
            }
            meterInputLeft_.assign(256, 0.0f);
            meterInputRight_.assign(256, 0.0f);
            meterOutputLeft_.assign(256, 0.0f);
            meterOutputRight_.assign(256, 0.0f);
            std::ostringstream stage;
            stage << "editor.meter.processor.ok inputs=" << meterInputBusCount_
                  << " outputs=" << meterOutputBusCount_
                  << " inCh=" << meterInputChannelCount_
                  << " outCh=" << meterOutputChannelCount_;
            logHostStage(stage.str());
        }

        void processEditorMeterAudio(double inputLeftPeak, double inputRightPeak) {
            if (suppressSyntheticEditorAudio_ ||
                meterProcessor_ == nullptr ||
                meterInputLeft_.empty() ||
                meterInputRight_.empty() ||
                meterOutputLeft_.empty() ||
                meterOutputRight_.empty()) {
                return;
            }
            constexpr int kFrames = 256;
            const float leftGain = static_cast<float>(std::clamp(inputLeftPeak, 0.0, 1.0));
            const float rightGain = static_cast<float>(std::clamp(inputRightPeak, 0.0, 1.0));
            for (int frame = 0; frame < kFrames; ++frame) {
                const float leftCarrier = std::sin(static_cast<float>(meterProbePhaseLeft_));
                const float rightCarrier = std::sin(static_cast<float>(meterProbePhaseRight_));
                constexpr double twoPi = 2.0 * M_PI;
                constexpr float probeTrim = 0.70710678f;
                meterProbePhaseLeft_ += twoPi * 440.0 / 48000.0;
                meterProbePhaseRight_ += twoPi * 587.33 / 48000.0;
                if (meterProbePhaseLeft_ > twoPi) {
                    meterProbePhaseLeft_ -= twoPi;
                }
                if (meterProbePhaseRight_ > twoPi) {
                    meterProbePhaseRight_ -= twoPi;
                }
                meterProbePhase_ += (2.0 * M_PI * 440.0) / 48000.0;
                if (meterProbePhase_ > 2.0 * M_PI) {
                    meterProbePhase_ -= 2.0 * M_PI;
                }
                if (meterInputChannelCount_ == 1) {
                    meterInputLeft_[static_cast<size_t>(frame)] = leftCarrier * ((leftGain + rightGain) * 0.5f) * probeTrim;
                    meterInputRight_[static_cast<size_t>(frame)] = 0.0f;
                } else {
                    meterInputLeft_[static_cast<size_t>(frame)] = leftCarrier * leftGain * probeTrim;
                    meterInputRight_[static_cast<size_t>(frame)] = rightCarrier * rightGain * probeTrim;
                }
                meterOutputLeft_[static_cast<size_t>(frame)] = 0.0f;
                meterOutputRight_[static_cast<size_t>(frame)] = 0.0f;
            }
            Steinberg::Vst::Sample32* inputChannels[2] = {meterInputLeft_.data(), meterInputRight_.data()};
            Steinberg::Vst::Sample32* outputChannels[2] = {meterOutputLeft_.data(), meterOutputRight_.data()};
            Steinberg::Vst::AudioBusBuffers inputBus {};
            Steinberg::Vst::AudioBusBuffers outputBus {};
            inputBus.numChannels = std::max(1, std::min(2, meterInputChannelCount_));
            inputBus.silenceFlags = (leftGain <= 0.000001f && rightGain <= 0.000001f)
                ? ((1 << inputBus.numChannels) - 1)
                : 0;
            inputBus.channelBuffers32 = inputChannels;
            outputBus.numChannels = std::max(1, std::min(2, meterOutputChannelCount_));
            outputBus.silenceFlags = 0;
            outputBus.channelBuffers32 = outputChannels;
            Steinberg::Vst::ProcessContext context {};
            context.state = Steinberg::Vst::ProcessContext::kPlaying |
                Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
                Steinberg::Vst::ProcessContext::kTempoValid |
                Steinberg::Vst::ProcessContext::kTimeSigValid |
                Steinberg::Vst::ProcessContext::kContTimeValid;
            context.sampleRate = 48000.0;
            context.projectTimeSamples = meterProbeSamples_;
            context.continousTimeSamples = meterProbeSamples_;
            context.projectTimeMusic = (static_cast<double>(meterProbeSamples_) / 48000.0) * 2.0;
            context.tempo = 120.0;
            context.timeSigNumerator = 4;
            context.timeSigDenominator = 4;
            EditorParameterChanges parameterChanges;
            parameterChanges.addControllerSnapshot(controller_);
            EditorParameterChanges outputParameterChanges;
            Steinberg::Vst::ProcessData processData {};
            processData.processMode = Steinberg::Vst::kRealtime;
            processData.symbolicSampleSize = Steinberg::Vst::kSample32;
            processData.numSamples = kFrames;
            processData.processContext = &context;
            processData.numInputs = meterInputBusCount_ > 0 ? 1 : 0;
            processData.numOutputs = meterOutputBusCount_ > 0 ? 1 : 0;
            processData.inputs = processData.numInputs > 0 ? &inputBus : nullptr;
            processData.outputs = processData.numOutputs > 0 ? &outputBus : nullptr;
            processData.inputParameterChanges = parameterChanges.getParameterCount() > 0 ? &parameterChanges : nullptr;
            processData.outputParameterChanges = &outputParameterChanges;
            try {
	                if (meterProcessor_->process(processData) == Steinberg::kResultOk && controller_ != nullptr) {
	                    for (const auto& [parameterId, value] : outputParameterChanges.latestValues()) {
	                        controller_->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(parameterId), value);
	                        forwardParameterValueToDaw(parameterId, value);
	                    }
	                }
            } catch (...) {
            }
            meterProbeSamples_ += kFrames;
        }

        // Process one real interleaved-stereo block from the engine through the
        // editor's own plugin instance. Used only in audio-bridge mode.
        bool processBridgeBlock(const float* input,
                                int frameCount,
                                const std::vector<neuracoust::daw::Vst3BridgeParam>& params,
                                float* output) {
            if (meterProcessor_ == nullptr || input == nullptr || output == nullptr) {
                return false;
            }
            if (frameCount <= 0 || frameCount > audioBridgeMaxBlock_ ||
                static_cast<int>(bridgeInLeft_.size()) < frameCount) {
                return false;
            }
            const int inCh = std::max(1, std::min(2, meterInputChannelCount_));
            const int outCh = std::max(1, std::min(2, meterOutputChannelCount_));
            for (int f = 0; f < frameCount; ++f) {
                const float l = input[static_cast<size_t>(f) * 2u];
                const float r = input[static_cast<size_t>(f) * 2u + 1u];
                if (inCh == 1) {
                    bridgeInLeft_[static_cast<size_t>(f)] = (l + r) * 0.5f;
                    bridgeInRight_[static_cast<size_t>(f)] = 0.0f;
                } else {
                    bridgeInLeft_[static_cast<size_t>(f)] = l;
                    bridgeInRight_[static_cast<size_t>(f)] = r;
                }
                bridgeOutLeft_[static_cast<size_t>(f)] = 0.0f;
                bridgeOutRight_[static_cast<size_t>(f)] = 0.0f;
            }
            Steinberg::Vst::Sample32* inChannels[2] = {bridgeInLeft_.data(), bridgeInRight_.data()};
            Steinberg::Vst::Sample32* outChannels[2] = {bridgeOutLeft_.data(), bridgeOutRight_.data()};
            Steinberg::Vst::AudioBusBuffers inputBus {};
            Steinberg::Vst::AudioBusBuffers outputBus {};
            inputBus.numChannels = inCh;
            inputBus.silenceFlags = 0;
            inputBus.channelBuffers32 = inChannels;
            outputBus.numChannels = outCh;
            outputBus.silenceFlags = 0;
            outputBus.channelBuffers32 = outChannels;
            Steinberg::Vst::ProcessContext context {};
            context.state = Steinberg::Vst::ProcessContext::kPlaying |
                Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
                Steinberg::Vst::ProcessContext::kTempoValid |
                Steinberg::Vst::ProcessContext::kTimeSigValid |
                Steinberg::Vst::ProcessContext::kSystemTimeValid |
                Steinberg::Vst::ProcessContext::kContTimeValid;
            context.sampleRate = audioBridgeSampleRate_;
            context.projectTimeSamples = bridgeProcessedSamples_;
            context.continousTimeSamples = bridgeProcessedSamples_;
            // A meter tick drains a whole burst of blocks in <1 ms of wall-clock, but a real-time
            // scope (FabFilter Pro-L) places its waveform by systemTime — feeding them all at "now"
            // stacked them at one x and left 16 ms gaps, a regular comb. Derive systemTime from the
            // sample position instead so the burst spreads across the scope exactly as it would at
            // real-time playback.
            context.systemTime = static_cast<Steinberg::int64>(
                (static_cast<double>(bridgeProcessedSamples_) / std::max(1.0, audioBridgeSampleRate_)) * 1.0e9);
            context.projectTimeMusic = (static_cast<double>(bridgeProcessedSamples_) /
                                        std::max(1.0, audioBridgeSampleRate_)) * 2.0;   // 120 BPM → beats
            context.tempo = 120.0;
            context.timeSigNumerator = 4;
            context.timeSigDenominator = 4;
            // Drive the meter instance with the controller's CURRENT parameter values — the same
            // thing the synthetic (probe) meter path does. The engine only streams changed params
            // into the shm on the first sub-block and then goes quiet, so `params` is empty once a
            // plug-in settles; feeding those alone left the meter component at its instantiation
            // defaults, so every parameter-dependent readout (Pro-C gain reduction, Pro-DS
            // reduction, Pro-L output/spectrum, Pro-MB spectrum) sat dead while only the raw input
            // analyzer drew. The controller already tracks the user's values (state sync at open +
            // PARAM_SET), so a full snapshot each block makes the meter self-sufficient.
            EditorParameterChanges parameterChanges;
            parameterChanges.addControllerSnapshot(controller_);
            EditorParameterChanges outputParameterChanges;
            Steinberg::Vst::ProcessData processData {};
            processData.processMode = Steinberg::Vst::kRealtime;
            processData.symbolicSampleSize = Steinberg::Vst::kSample32;
            processData.numSamples = frameCount;
            processData.processContext = &context;
            processData.numInputs = meterInputBusCount_ > 0 ? 1 : 0;
            processData.numOutputs = meterOutputBusCount_ > 0 ? 1 : 0;
            processData.inputs = processData.numInputs > 0 ? &inputBus : nullptr;
            processData.outputs = processData.numOutputs > 0 ? &outputBus : nullptr;
            processData.inputParameterChanges = parameterChanges.getParameterCount() > 0 ? &parameterChanges : nullptr;
            processData.outputParameterChanges = &outputParameterChanges;
            Steinberg::tresult result = Steinberg::kResultFalse;
            try {
                result = meterProcessor_->process(processData);
            } catch (...) {
                return false;
            }
            if (result != Steinberg::kResultOk) {
                return false;
            }
            // Forward any output (read-only meter) parameters the plug-in wrote back to the
            // controller so GUIs that draw meters from output parameters update — same as the
            // synthetic path.
            if (controller_ != nullptr) {
                for (const auto& [parameterId, value] : outputParameterChanges.latestValues()) {
                    controller_->setParamNormalized(static_cast<Steinberg::Vst::ParamID>(parameterId), value);
                }
            }
            const bool haveOutput = meterOutputBusCount_ > 0 && outCh > 0;
            for (int f = 0; f < frameCount; ++f) {
                float outLeft;
                float outRight;
                if (!haveOutput) {
                    outLeft = bridgeInLeft_[static_cast<size_t>(f)];
                    outRight = inCh == 1 ? bridgeInLeft_[static_cast<size_t>(f)] : bridgeInRight_[static_cast<size_t>(f)];
                } else {
                    outLeft = bridgeOutLeft_[static_cast<size_t>(f)];
                    outRight = outCh == 1 ? bridgeOutLeft_[static_cast<size_t>(f)] : bridgeOutRight_[static_cast<size_t>(f)];
                }
                output[static_cast<size_t>(f) * 2u] = outLeft;
                output[static_cast<size_t>(f) * 2u + 1u] = outRight;
            }
            bridgeProcessedSamples_ += frameCount;
            return true;
        }

        void connectComponentAndController() {
            if (component_ == nullptr || controller_ == nullptr || componentControllerConnected_) {
                return;
            }
            void* componentConnection = nullptr;
            void* controllerConnection = nullptr;
            if (component_->queryInterface(Steinberg::Vst::IConnectionPoint::iid, &componentConnection) != Steinberg::kResultOk ||
                controller_->queryInterface(Steinberg::Vst::IConnectionPoint::iid, &controllerConnection) != Steinberg::kResultOk ||
                componentConnection == nullptr ||
                controllerConnection == nullptr) {
                if (componentConnection != nullptr) {
                    static_cast<Steinberg::Vst::IConnectionPoint*>(componentConnection)->release();
                }
                if (controllerConnection != nullptr) {
                    static_cast<Steinberg::Vst::IConnectionPoint*>(controllerConnection)->release();
                }
                return;
            }
            componentConnection_ = static_cast<Steinberg::Vst::IConnectionPoint*>(componentConnection);
            controllerConnection_ = static_cast<Steinberg::Vst::IConnectionPoint*>(controllerConnection);
            const bool componentAccepted = componentConnection_->connect(controllerConnection_) == Steinberg::kResultOk;
            const bool controllerAccepted = controllerConnection_->connect(componentConnection_) == Steinberg::kResultOk;
            componentControllerConnected_ = componentAccepted && controllerAccepted;
        }

        void disconnectComponentAndController() {
            if (componentConnection_ != nullptr && controllerConnection_ != nullptr) {
                if (componentControllerConnected_) {
                    componentConnection_->disconnect(controllerConnection_);
                    controllerConnection_->disconnect(componentConnection_);
                }
                componentConnection_->release();
                controllerConnection_->release();
            } else {
                if (componentConnection_ != nullptr) {
                    componentConnection_->release();
                }
                if (controllerConnection_ != nullptr) {
                    controllerConnection_->release();
                }
            }
            componentConnection_ = nullptr;
            controllerConnection_ = nullptr;
            componentControllerConnected_ = false;
        }

	    bool createComponent(Steinberg::IPluginFactory* factory, const Vst3PluginDescriptor& descriptor) {
        const int classCount = factory->countClasses();
        auto tryCreateAt = [&](int index) -> bool {
            Steinberg::PClassInfo info;
            try {
                if (factory->getClassInfo(index, &info) != Steinberg::kResultOk) {
                    return false;
                }
            } catch (...) {
                return false;
            }
            const std::string category(info.category);
            if (category != "Audio Module Class" && category != "Audio Effect Class") {
                return false;
            }
            void* object = nullptr;
            try {
                if (factory->createInstance(info.cid, Steinberg::Vst::IComponent::iid, &object) != Steinberg::kResultOk || object == nullptr) {
                    return false;
                }
            } catch (...) {
                return false;
            }
            component_ = static_cast<Steinberg::Vst::IComponent*>(object);
            try {
                if (component_->initialize(static_cast<Steinberg::Vst::IHostApplication*>(host_)) == Steinberg::kResultOk) {
                    componentInitialized_ = true;
                }
            } catch (...) {
                return false;
            }
            Steinberg::TUID controllerCid {};
            try {
                if (component_->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
                    std::memcpy(controllerCid_, controllerCid, sizeof(controllerCid_));
                    hasControllerCid_ = true;
                    if (std::getenv("NEURACOUST_VST3_EDITOR_LOG_CLASSES") != nullptr) {
                        std::cerr << "HOST_CONTROLLER_CID " << tuidToHex(controllerCid_) << std::endl;
                    }
                }
            } catch (...) {
                hasControllerCid_ = false;
            }
            return true;
        };

        if (!descriptor.name.empty() || !descriptor.componentClassName.empty()) {
            for (int index = 0; index < classCount; ++index) {
                Steinberg::PClassInfo info;
                try {
                    if (factory->getClassInfo(index, &info) != Steinberg::kResultOk) {
                        continue;
                    }
                } catch (...) {
                    continue;
                }
                const std::string category(info.category);
                if ((category == "Audio Module Class" || category == "Audio Effect Class") &&
                    classMatchesDescriptor(info, descriptor) &&
                    tryCreateAt(index)) {
                    const std::string stage = "component.selected " + classInfoSummary(info);
                    logHostStage(stage.c_str());
                    return true;
                }
            }
            const auto wavesIdentity = lowercaseAscii(descriptor.brand + " " +
                                                      descriptor.vendor + " " +
                                                      descriptor.bundlePath + " " +
                                                      descriptor.executablePath + " " +
                                                      descriptor.name + " " +
                                                      descriptor.componentClassName);
            if (wavesIdentity.find("waves") != std::string::npos ||
                wavesIdentity.find("waveshell") != std::string::npos) {
                logHostStage("component.waves.no_matching_class");
                return false;
            }
        }

        for (int index = 0; index < classCount; ++index) {
            if (tryCreateAt(index)) {
                Steinberg::PClassInfo info;
                if (factory->getClassInfo(index, &info) == Steinberg::kResultOk) {
                    const std::string stage = "component.fallback " + classInfoSummary(info);
                    logHostStage(stage.c_str());
                }
                return true;
            }
        }
        return false;
    }

    bool createController(Steinberg::IPluginFactory* factory, std::string& error) {
        const bool preferComponentController = std::getenv("NEURACOUST_VST3_EDITOR_PREFER_COMPONENT_CONTROLLER") != nullptr;
        auto tryComponentController = [&]() -> bool {
            if (component_ == nullptr) {
                return false;
            }
            void* object = nullptr;
            try {
                if (component_->queryInterface(Steinberg::Vst::IEditController::iid, &object) == Steinberg::kResultOk && object != nullptr) {
                    controller_ = static_cast<Steinberg::Vst::IEditController*>(object);
                    controllerFromComponent_ = true;
                    return true;
                }
            } catch (...) {
                error = "VST3 component threw while querying its edit controller.";
                return false;
            }
            return false;
        };
        if (preferComponentController && tryComponentController()) {
            return true;
        }
        if (hasControllerCid_) {
            void* object = nullptr;
            try {
                if (factory->createInstance(controllerCid_, Steinberg::Vst::IEditController::iid, &object) == Steinberg::kResultOk && object != nullptr) {
                    controller_ = static_cast<Steinberg::Vst::IEditController*>(object);
                    return true;
                }
            } catch (const std::exception& exception) {
                error = std::string("VST3 edit controller threw during creation: ") + exception.what();
                return false;
            } catch (...) {
                error = "VST3 edit controller threw during creation.";
                return false;
            }
        }
        if (tryComponentController()) {
            return true;
        }
        const int classCount = factory->countClasses();
        for (int index = 0; index < classCount; ++index) {
            Steinberg::PClassInfo info;
            try {
                if (factory->getClassInfo(index, &info) != Steinberg::kResultOk) {
                    continue;
                }
            } catch (...) {
                continue;
            }
            if (std::string(info.category) != "Component Controller Class") {
                continue;
            }
            void* object = nullptr;
            try {
                if (factory->createInstance(info.cid, Steinberg::Vst::IEditController::iid, &object) == Steinberg::kResultOk && object != nullptr) {
                    controller_ = static_cast<Steinberg::Vst::IEditController*>(object);
                    return true;
                }
            } catch (const std::exception& exception) {
                error = std::string("VST3 edit controller threw during creation: ") + exception.what();
                return false;
            } catch (...) {
                error = "VST3 edit controller threw during creation.";
                return false;
            }
        }
        error = "No VST3 edit controller class could be instantiated.";
        return false;
    }

	    void* module_ = nullptr;
        CFBundleRef bundle_ = nullptr;
	    EditorHostSupport* host_ = nullptr;
	    Steinberg::Vst::IComponent* component_ = nullptr;
	    Steinberg::Vst::IEditController* controller_ = nullptr;
        Steinberg::Vst::IAudioProcessor* meterProcessor_ = nullptr;
        Steinberg::Vst::IConnectionPoint* componentConnection_ = nullptr;
        Steinberg::Vst::IConnectionPoint* controllerConnection_ = nullptr;
	    Steinberg::IPlugView* view_ = nullptr;
    NSPanel* panel_ = nil;
    NAMeterTelemetryOverlayView* meterOverlay_ = nil;
    id closeObserver_ = nil;
    id resizeObserver_ = nil;
    id flexibleResizeEventMonitor_ = nil;
    id transportKeyMonitor_ = nil;
        Vst3PluginDescriptor descriptor_;
        Steinberg::TUID controllerCid_ {};
        std::string resourceOverlayDirectory_;
        std::string resourceBundleOverlayDirectory_;
        std::vector<std::filesystem::path> mirroredResourcePaths_;
        bool hasControllerCid_ = false;
	    bool componentInitialized_ = false;
	    bool controllerInitialized_ = false;
        bool controllerFromComponent_ = false;
        bool componentControllerConnected_ = false;
	    bool viewAttached_ = false;
        using BundleExitFn = bool (*)();
        BundleExitFn bundleExit_ = nullptr;
        bool bundleEntryCalled_ = false;
        bool viewCanResize_ = false;
        bool trustRequestedResize_ = false;
        bool allowComponentStateTransfer_ = false;
	        bool suppressWindowResizeCallback_ = false;
	        bool hasCurrentSize_ = false;
	        bool flexibleInitialSizeCapActive_ = false;
	        bool wavesEditor_ = false;
	        bool flexibleWavesRs124_ = false;
	        bool suppressSyntheticEditorAudio_ = false;
        Steinberg::int32 flexibleInitialSizeCapHeight_ = 0;
        bool meterOverlayEnabled_ = false;
        std::vector<float> meterInputLeft_;
        std::vector<float> meterInputRight_;
        std::vector<float> meterOutputLeft_;
        std::vector<float> meterOutputRight_;
        int meterInputBusCount_ = 0;
        int meterOutputBusCount_ = 0;
        int meterInputChannelCount_ = 2;
        int meterOutputChannelCount_ = 2;
        double meterProbePhase_ = 0.0;
        double meterProbePhaseLeft_ = 0.0;
        double meterProbePhaseRight_ = M_PI * 0.5;
        int64_t meterProbeSamples_ = 0;
        double latestMeterInputLeft_ = 0.0;
        double latestMeterInputRight_ = 0.0;
        double latestMeterOutputLeft_ = 0.0;
        double latestMeterOutputRight_ = 0.0;
        NSTimeInterval lastMeterUpdateSeconds_ = 0.0;
        NSTimeInterval lastMeterProcessSeconds_ = 0.0;
        std::map<uint32_t, double> lastPolledParameterValues_;
        NSTimeInterval lastParameterSnapshotSeconds_ = 0.0;
        Steinberg::ViewRect currentSizeRect_ {0, 0, 0, 0};

        // Out-of-process realtime bridge: when set, this editor host is the audio
        // server for its insert, so the SAME instance the user sees also processes
        // the real track audio (and its own meters reflect it).
        bool audioBridgeActive_ = false;
        dispatch_source_t bridgeMeterTimer_ = nullptr;   // fast (~2 ms) main-thread pump for real-time-paced metering
        int bridgeEmptyDrainTicks_ = 0;   // consecutive empty meter ticks; feed decay-silence only once genuinely stopped
        bool audioBridgeObserver_ = false;
        std::string audioBridgeShmName_;
        int audioBridgeMaxBlock_ = 256;
        double audioBridgeSampleRate_ = 48000.0;
        std::thread audioBridgeThread_;
        std::atomic<bool> audioBridgeStop_ {false};
        int64_t bridgeProcessedSamples_ = 0;
        std::vector<float> bridgeInLeft_;
        std::vector<float> bridgeInRight_;
        std::vector<float> bridgeOutLeft_;
        std::vector<float> bridgeOutRight_;
        // Observer capture queue: the background thread reads the engine's bridge and PUSHES each
        // fresh block here; the main-thread meter tick DRAINS and runs the plug-in's process() so
        // its gain-reduction / analyzer notify() lands on the GUI thread. Blocks are kept in order
        // so a spectrum analyzer sees a continuous stream; the queue is capped so a stalled UI
        // can't grow it without bound.
        struct BridgeCapturedBlock {
            std::vector<float> input;   // interleaved stereo, frameCount * 2
            int frameCount = 0;
            std::vector<neuracoust::daw::Vst3BridgeParam> params;
        };
        std::mutex bridgeCaptureMutex_;
        std::deque<BridgeCapturedBlock> bridgeCaptureQueue_;
        static constexpr size_t kBridgeCaptureQueueCap = 64;   // ~340 ms at 256/48k; drop oldest past this
	};
#endif

} // namespace

int main(int argc, const char* argv[]) {
	    @autoreleasepool {
	        const std::string pluginPath = argumentValue(argc, argv, "--plugin");
	        const std::string pluginName = argumentValue(argc, argv, "--name");
	        const std::string title = argumentValue(argc, argv, "--title");
            const std::string pluginClassId = argumentValue(argc, argv, "--class-id");
            const std::string pluginClassName = argumentValue(argc, argv, "--class-name");
            const bool probeMode = hasArgument(argc, argv, "--probe");
            const bool inspectParametersMode = hasArgument(argc, argv, "--inspect-parameters");
            const bool meterOverlayEnabled = hasArgument(argc, argv, "--meter-overlay");
            // --observe-shm: read-only observer that animates the plugin's own
            // meters/graphics from the engine's real per-insert audio (safe; does
            // not touch the audio path). --audio-bridge-shm: full server mode.
            const std::string observeShm = argumentValue(argc, argv, "--observe-shm");
            const bool bridgeObserver = !observeShm.empty();
            const std::string audioBridgeShm = bridgeObserver
                ? observeShm
                : argumentValue(argc, argv, "--audio-bridge-shm");
            const int audioBridgeMaxBlock = audioBridgeShm.empty()
                ? 0
                : std::max(1, std::atoi(argumentValue(argc, argv, bridgeObserver ? "--observe-max-block" : "--audio-bridge-max-block").c_str()));
            const double audioBridgeSampleRate = audioBridgeShm.empty()
                ? 48000.0
                : std::strtod(argumentValue(argc, argv, bridgeObserver ? "--observe-sample-rate" : "--audio-bridge-sample-rate").c_str(), nullptr);
#if defined(NEURACOUST_HAS_VST3_SDK)
            if (hasArgument(argc, argv, "--self-test-parameter-polling")) {
                return runParameterPollingSelfTest();
            }
	            if (hasArgument(argc, argv, "--self-test-waves-view-size")) {
	                return runWavesViewSizeSelfTest();
	            }
	            if (hasArgument(argc, argv, "--self-test-synthetic-audio-policy")) {
	                return runSyntheticEditorAudioPolicySelfTest();
	            }
	#endif
	        if (pluginPath.empty()) {
            showFatalAlert(@"VST3 플러그인 창을 열 수 없습니다", @"플러그인 경로가 전달되지 않았습니다.");
            return 2;
        }
#if !defined(NEURACOUST_HAS_VST3_SDK)
        showFatalAlert(@"VST3 플러그인 창을 열 수 없습니다", @"현재 빌드는 Steinberg VST3 SDK 없이 구성되었습니다.");
        return 3;
#else
        Vst3PluginDescriptor descriptor = resolveVst3PluginDescriptorForInsert(pluginName,
                                                                               pluginPath,
                                                                               pluginClassId,
                                                                               pluginClassName);
        const std::string resolvedStage = "descriptor.resolved name=" + descriptor.name +
            " classId=" + descriptor.componentClassCid +
            " className=" + descriptor.componentClassName +
            " bundle=" + descriptor.bundlePath;
        logHostStage(resolvedStage.c_str());
        if (descriptor.executablePath.empty() && descriptor.bundlePath.empty()) {
            descriptor.bundlePath = pluginPath;
        }
        if (inspectParametersMode) {
            int limit = 64;
            const std::string limitText = argumentValue(argc, argv, "--limit");
            if (!limitText.empty()) {
                limit = std::max(0, std::min(512, std::atoi(limitText.c_str())));
            }
            return inspectParametersAndPrint(descriptor, limit);
        }
        if (probeMode && descriptor.bundlePath.empty() && descriptor.executablePath.empty()) {
            std::cerr << "HOST_ERROR Could not resolve VST3 descriptor for probe." << std::endl;
            return 4;
        }
        logHostStage("app.begin");
	        [NSApplication sharedApplication];
        logHostStage("app.shared.ok");
	        // Regular, not Accessory: an Accessory (background-helper) process can't reliably pull its
		        // window in front of the DAW on modern macOS, so the editor kept opening hidden BEHIND
		        // the main window. Regular lets activateIgnoringOtherApps + orderFront actually raise it.
		        // Cost is a Dock icon per open editor, acceptable for a window the user works in.
		        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        logHostStage("app.activation.ok");
        // Defeat App Nap. Live metering runs on a 1/30 s main-thread timer; when this editor's
        // window is behind the DAW, App Nap throttles background timers to a crawl and the meters
        // (gain reduction, analyzer, output level) freeze — only the frontmost editor kept moving.
        // A held NSActivity with a latency-critical option keeps the timer at full rate. The token
        // is intentionally leaked for the process lifetime (one editor = one process).
        static id sAppNapToken = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                              reason:@"live plug-in metering"];
        (void)sAppNapToken;
        // While a plug-in editor window has focus, the spacebar goes to this
        // process instead of the DAW, so transport wouldn't toggle. Forward an
        // unmodified spacebar to the DAW over stdout (unless a text field is being
        // edited, where the space should be typed).
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* keyEvent) {
            NSString* characters = keyEvent.charactersIgnoringModifiers ?: @"";
            const NSEventModifierFlags mods = keyEvent.modifierFlags &
                (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl | NSEventModifierFlagShift);
            if (characters.length != 1 || [characters characterAtIndex:0] != ' ' || mods != 0) {
                return keyEvent;
            }
            id responder = keyEvent.window.firstResponder;
            if ([responder isKindOfClass:[NSTextView class]] ||
                [responder isKindOfClass:[NSTextField class]] ||
                [responder isKindOfClass:[NSComboBox class]] ||
                [responder isKindOfClass:[NSSearchField class]]) {
                return keyEvent;
            }
            if (!keyEvent.isARepeat) {
                std::cout << "TRANSPORT_TOGGLE" << std::endl;
            }
            return (NSEvent*)nil;
        }];
        if (std::getenv("NEURACOUST_VST3_EDITOR_DEFER_OPEN") != nullptr) {
            __block Vst3EditorSession* deferredSession = new Vst3EditorSession();
            NSString* nsTitle = title.empty()
                ? [NSString stringWithFormat:@"%s", pluginName.empty() ? "VST3 Plug-in" : pluginName.c_str()]
                : [NSString stringWithUTF8String:title.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{
                std::string deferredError;
                deferredSession->configureAudioBridge(audioBridgeShm, audioBridgeMaxBlock, audioBridgeSampleRate, bridgeObserver);
                @try {
                    try {
                        if (!deferredSession->open(descriptor, nsTitle, meterOverlayEnabled, deferredError)) {
                            std::cerr << "HOST_ERROR " << deferredError << std::endl;
                            _Exit(4);
                        }
                    } catch (const std::exception& exception) {
                        std::cerr << "HOST_ERROR Plug-in exception while creating native editor: " << exception.what() << std::endl;
                        _Exit(5);
                    } catch (...) {
                        std::cerr << "HOST_ERROR Unknown plug-in exception while creating native editor." << std::endl;
                        _Exit(6);
                    }
                } @catch (NSException* exception) {
                    std::cerr << "HOST_ERROR Objective-C plug-in exception while creating native editor: "
                              << (exception.reason != nil ? exception.reason.UTF8String : "unknown") << std::endl;
                    _Exit(7);
                }
                deferredSession->initializeParameterPolling();
                deferredSession->startAudioBridge();
                std::cout << "READY" << std::endl;
                if (probeMode) {
                    _Exit(0);
                }
                __block Vst3EditorSession* pollingSession = deferredSession;
                [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                                repeats:YES
                                                  block:^(NSTimer*) {
                    if (pollingSession != nullptr) {
                        pollingSession->pollControllerParameterChanges();
                        pollingSession->tickEditorMeterProcessor();
                    }
                }];
                [NSApp activateIgnoringOtherApps:YES];
            });
            [NSApp run];
            delete deferredSession;
            _Exit(0);
        }
        std::string error;
        auto session = std::make_unique<Vst3EditorSession>();
        session->configureAudioBridge(audioBridgeShm, audioBridgeMaxBlock, audioBridgeSampleRate, bridgeObserver);
        NSString* nsTitle = title.empty()
            ? [NSString stringWithFormat:@"%s", pluginName.empty() ? "VST3 Plug-in" : pluginName.c_str()]
            : [NSString stringWithUTF8String:title.c_str()];
                @try {
                    try {
		                if (!session->open(descriptor, nsTitle, meterOverlayEnabled, error)) {
                            std::cerr << "HOST_ERROR " << error << std::endl;
		                    return 4;
		                }
                    } catch (const std::exception& exception) {
                        std::cerr << "HOST_ERROR Plug-in exception while creating native editor: " << exception.what() << std::endl;
                        return 5;
                    } catch (...) {
                        std::cerr << "HOST_ERROR Unknown plug-in exception while creating native editor." << std::endl;
                        return 6;
		                }
                } @catch (NSException* exception) {
                    std::cerr << "HOST_ERROR Objective-C plug-in exception while creating native editor: "
                              << (exception.reason != nil ? exception.reason.UTF8String : "unknown") << std::endl;
                    return 7;
                }
                __block NSMutableData* meterInputBuffer = [[NSMutableData alloc] init];
                __block Vst3EditorSession* sessionForMeters = session.get();
                __block Vst3EditorSession* sessionForParameterPolling = session.get();
                session->initializeParameterPolling();
                session->startAudioBridge();
                NSTimer* parameterPollTimer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0)
                                                                              repeats:YES
                                                                                block:^(NSTimer*) {
                    if (sessionForParameterPolling != nullptr) {
                        sessionForParameterPolling->pollControllerParameterChanges();
                        sessionForParameterPolling->tickEditorMeterProcessor();
                    }
                }];
                NSFileHandle* meterInput = [NSFileHandle fileHandleWithStandardInput];
                meterInput.readabilityHandler = ^(NSFileHandle* handle) {
                    NSData* data = [handle availableData];
                    if (data.length == 0) {
                        handle.readabilityHandler = nil;
                        return;
                    }
                    [meterInputBuffer appendData:data];
                    while (true) {
                        const char* bytes = static_cast<const char*>(meterInputBuffer.bytes);
                        const NSUInteger length = meterInputBuffer.length;
                        const void* newline = memchr(bytes, '\n', length);
                        if (newline == nullptr) {
                            break;
                        }
                        const NSUInteger lineLength = static_cast<const char*>(newline) - bytes;
                        NSData* lineData = [meterInputBuffer subdataWithRange:NSMakeRange(0, lineLength)];
                        NSString* line = [[[NSString alloc] initWithData:lineData encoding:NSUTF8StringEncoding] autorelease];
                        [meterInputBuffer replaceBytesInRange:NSMakeRange(0, lineLength + 1) withBytes:nullptr length:0];
                        if ([line hasPrefix:@"PARAM_SET "]) {
                            NSArray<NSString*>* parts = [line componentsSeparatedByString:@" "];
                            if (parts.count >= 3) {
                                const uint32_t parameterId = static_cast<uint32_t>(std::strtoul(parts[1].UTF8String, nullptr, 10));
                                const double value = std::strtod(parts[2].UTF8String, nullptr);
                                dispatch_async(dispatch_get_main_queue(), ^{
                                    if (sessionForMeters != nullptr) {
                                        sessionForMeters->setHostParameterValue(parameterId, value);
                                    }
                                });
                            }
                            continue;
                        }
                        if (![line hasPrefix:@"METER "]) {
                            continue;
                        }
                        NSArray<NSString*>* parts = [line componentsSeparatedByString:@" "];
                        if (parts.count < 5) {
                            continue;
                        }
                        const double inputLeft = std::strtod(parts[1].UTF8String, nullptr);
                        const double inputRight = std::strtod(parts[2].UTF8String, nullptr);
                        const double outputLeft = std::strtod(parts[3].UTF8String, nullptr);
                        const double outputRight = std::strtod(parts[4].UTF8String, nullptr);
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (sessionForMeters != nullptr) {
                                sessionForMeters->setExternalMeterLevels(inputLeft, inputRight, outputLeft, outputRight);
                            }
                        });
                    }
                };
		        std::cout << "READY" << std::endl;
                if (probeMode) {
                    // Some commercial VST3 editors crash while being explicitly
                    // terminated/unloaded after a successful native attach. This
                    // helper is a short-lived process, so let the OS reclaim the
                    // plug-in state after a successful probe instead of turning a
                    // valid editor launch into a false crash.
                    session.release();
                    _Exit(0);
                }
		        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        logHostStage("app.run.returned");
        sessionForParameterPolling = nullptr;
        [parameterPollTimer invalidate];
        meterInput.readabilityHandler = nil;
        [meterInputBuffer release];
        // The helper owns one native editor window. Once that window closes the
        // process can exit directly; explicit VST teardown is less stable across
        // vendors than process teardown.
        session.release();
        _Exit(0);
#endif
    }
}
