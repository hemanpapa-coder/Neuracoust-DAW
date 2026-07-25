#include "control/ControlSurfaceMidi.h"

#if defined(__APPLE__)
#import <CoreMIDI/CoreMIDI.h>
#include <algorithm>
#include <mutex>
#include <sstream>

namespace neuracoust::daw {
namespace {
std::string property(MIDIObjectRef object, CFStringRef key) {
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, key, &value) != noErr || value == nullptr) return {};
    char text[512]{};
    const bool ok = CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(value);
    return ok ? text : "";
}
std::string endpointId(MIDIEndpointRef endpoint, ItemCount index, bool output) {
    SInt32 uniqueId = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uniqueId) == noErr && uniqueId)
        return std::to_string(uniqueId);
    std::ostringstream s; s << static_cast<uint64_t>(endpoint) << (output ? "-out-" : "-in-") << index;
    return s.str();
}
std::vector<ControlSurfaceMidiEndpoint> enumerate(bool output) {
    std::vector<ControlSurfaceMidiEndpoint> result;
    const ItemCount count = output ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef endpoint = output ? MIDIGetDestination(i) : MIDIGetSource(i);
        if (!endpoint) continue;
        auto name = property(endpoint, kMIDIPropertyDisplayName);
        if (name.empty()) name = property(endpoint, kMIDIPropertyName);
        result.push_back({endpointId(endpoint, i, output), name.empty() ? "MIDI" : name});
    }
    return result;
}
MIDIEndpointRef findEndpoint(const std::string& id, bool output) {
    const ItemCount count = output ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef endpoint = output ? MIDIGetDestination(i) : MIDIGetSource(i);
        if (endpoint && endpointId(endpoint, i, output) == id) return endpoint;
    }
    return 0;
}
}

class ControlSurfaceMidi::Impl {
public:
    ~Impl() { disconnect(); }
    std::vector<ControlSurfaceMidiEndpoint> inputs() const { return enumerate(false); }
    std::vector<ControlSurfaceMidiEndpoint> outputs() const { return enumerate(true); }

    bool connect(const std::string& inputId, const std::string& outputId) {
        disconnect();
        inputId_ = inputId; outputId_ = outputId;
        input_ = findEndpoint(inputId, false);
        output_ = findEndpoint(outputId, true);
        if (!input_ || !output_) { status_ = "선택한 HUI MIDI 포트를 찾을 수 없습니다."; return false; }
        if (MIDIClientCreate(CFSTR("Neuracoust HUI"), &Impl::notify, this, &client_) != noErr ||
            MIDIInputPortCreate(client_, CFSTR("HUI Input"), &Impl::read, this, &inputPort_) != noErr ||
            MIDIOutputPortCreate(client_, CFSTR("HUI Output"), &outputPort_) != noErr ||
            MIDIPortConnectSource(inputPort_, input_, nullptr) != noErr) {
            status_ = "HUI MIDI 포트를 열 수 없습니다."; disconnect(); return false;
        }
        connected_ = true; status_ = "Mackie HUI 연결됨";
        return true;
    }
    void disconnect() {
        connected_ = false;
        if (inputPort_ && input_) MIDIPortDisconnectSource(inputPort_, input_);
        if (inputPort_) MIDIPortDispose(inputPort_);
        if (outputPort_) MIDIPortDispose(outputPort_);
        if (client_) MIDIClientDispose(client_);
        client_ = 0; inputPort_ = 0; outputPort_ = 0; input_ = 0; output_ = 0;
        std::lock_guard<std::mutex> lock(mutex_); messages_.clear(); sysex_.clear();
    }
    bool connected() const { return connected_; }
    std::string statusMessage() const { return status_; }
    std::vector<std::vector<std::uint8_t>> consume() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = std::move(messages_); messages_.clear(); return result;
    }
    bool send(const std::vector<std::uint8_t>& bytes) {
        if (!connected_ || !outputPort_ || !output_ || bytes.empty()) return false;
        Byte storage[1024]{};
        auto* list = reinterpret_cast<MIDIPacketList*>(storage);
        MIDIPacket* packet = MIDIPacketListInit(list);
        packet = MIDIPacketListAdd(list, sizeof(storage), packet, 0,
                                   static_cast<UInt16>(std::min<size_t>(bytes.size(), 900)), bytes.data());
        return packet && MIDISend(outputPort_, output_, list) == noErr;
    }

private:
    static void read(const MIDIPacketList* list, void* context, void*) {
        if (context) static_cast<Impl*>(context)->handle(list);
    }
    static void notify(const MIDINotification* note, void* context) {
        if (!context || !note) return;
        auto* self = static_cast<Impl*>(context);
        if (note->messageID != kMIDIMsgSetupChanged &&
            note->messageID != kMIDIMsgObjectAdded && note->messageID != kMIDIMsgObjectRemoved) return;
        if (!self->connected_) return;
        if (!findEndpoint(self->inputId_, false) || !findEndpoint(self->outputId_, true)) {
            self->connected_ = false;
            self->status_ = "HUI 장치 연결이 끊어졌습니다.";
        }
    }
    void handle(const MIDIPacketList* list) {
        if (!list) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const MIDIPacket* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; ++p) {
            parse(packet->data, packet->length);
            packet = MIDIPacketNext(packet);
        }
    }
    void parse(const Byte* data, UInt16 length) {
        size_t i = 0;
        while (i < length) {
            const std::uint8_t status = data[i];
            if (!sysex_.empty() || status == 0xf0) {
                if (sysex_.empty()) sysex_.push_back(0xf0), ++i;
                while (i < length) {
                    sysex_.push_back(data[i]);
                    if (data[i++] == 0xf7) { messages_.push_back(sysex_); sysex_.clear(); break; }
                }
                continue;
            }
            if (status >= 0xf8) { messages_.push_back({status}); ++i; continue; }
            const int size = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 2 : 3;
            if ((status & 0x80) && i + size <= length) {
                messages_.emplace_back(data + i, data + i + size); i += size;
            } else ++i;
        }
    }

    MIDIClientRef client_ = 0; MIDIPortRef inputPort_ = 0, outputPort_ = 0;
    MIDIEndpointRef input_ = 0, output_ = 0;
    std::string inputId_, outputId_, status_ = "연결 안 됨";
    bool connected_ = false;
    mutable std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> messages_;
    std::vector<std::uint8_t> sysex_;
};

ControlSurfaceMidi::ControlSurfaceMidi() : impl_(std::make_unique<Impl>()) {}
ControlSurfaceMidi::~ControlSurfaceMidi() = default;
std::vector<ControlSurfaceMidiEndpoint> ControlSurfaceMidi::inputs() const { return impl_->inputs(); }
std::vector<ControlSurfaceMidiEndpoint> ControlSurfaceMidi::outputs() const { return impl_->outputs(); }
bool ControlSurfaceMidi::connect(const std::string& i, const std::string& o) { return impl_->connect(i, o); }
void ControlSurfaceMidi::disconnect() { impl_->disconnect(); }
bool ControlSurfaceMidi::connected() const { return impl_->connected(); }
std::string ControlSurfaceMidi::statusMessage() const { return impl_->statusMessage(); }
std::vector<std::vector<std::uint8_t>> ControlSurfaceMidi::consumeMessages() { return impl_->consume(); }
bool ControlSurfaceMidi::send(const std::vector<std::uint8_t>& b) { return impl_->send(b); }
}
#endif
