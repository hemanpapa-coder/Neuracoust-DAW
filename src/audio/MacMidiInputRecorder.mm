#include "audio/MidiInputRecorder.h"

#if defined(__APPLE__)
#import <CoreAudio/CoreAudio.h>
#import <CoreMIDI/CoreMIDI.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>

namespace neuracoust::daw {

namespace {

std::string stringFromMidiObjectProperty(MIDIObjectRef object, CFStringRef property) {
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr) {
        return {};
    }
    char buffer[512] {};
    const bool ok = CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(value);
    return ok ? std::string(buffer) : std::string();
}

std::string sourceIdForEndpoint(MIDIEndpointRef endpoint, ItemCount index) {
    SInt32 uniqueId = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uniqueId) == noErr && uniqueId != 0) {
        return std::to_string(uniqueId);
    }
    std::ostringstream out;
    out << static_cast<uint64_t>(endpoint) << "-" << index;
    return out.str();
}

std::string destinationIdForEndpoint(MIDIEndpointRef endpoint, ItemCount index) {
    SInt32 uniqueId = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uniqueId) == noErr && uniqueId != 0) {
        return std::to_string(uniqueId);
    }
    std::ostringstream out;
    out << static_cast<uint64_t>(endpoint) << "-out-" << index;
    return out.str();
}

double hostTimeToSeconds(uint64_t hostTime) {
    if (hostTime == 0) {
        return 0.0;
    }
    return static_cast<double>(AudioConvertHostTimeToNanos(hostTime)) / 1000000000.0;
}

} // namespace

class MidiInputRecorder::Impl {
public:
    ~Impl() {
        std::vector<RecordedMidiEvent> ignoredEvents;
        std::string ignoredError;
        stop(ignoredEvents, ignoredError);
    }

    std::vector<MidiInputSourceInfo> availableInputs() const {
        std::vector<MidiInputSourceInfo> inputs;
        const ItemCount count = MIDIGetNumberOfSources();
        for (ItemCount index = 0; index < count; ++index) {
            MIDIEndpointRef source = MIDIGetSource(index);
            if (source == 0) {
                continue;
            }
            MidiInputSourceInfo info;
            info.id = sourceIdForEndpoint(source, index);
            info.name = stringFromMidiObjectProperty(source, kMIDIPropertyDisplayName);
            if (info.name.empty()) {
                info.name = stringFromMidiObjectProperty(source, kMIDIPropertyName);
            }
            if (info.name.empty()) {
                info.name = "MIDI Input";
            }
            inputs.push_back(info);
        }
        return inputs;
    }

    std::vector<MidiInputSourceInfo> availableOutputs() const {
        std::vector<MidiInputSourceInfo> outputs;
        const ItemCount count = MIDIGetNumberOfDestinations();
        for (ItemCount index = 0; index < count; ++index) {
            MIDIEndpointRef destination = MIDIGetDestination(index);
            if (destination == 0) {
                continue;
            }
            MidiInputSourceInfo info;
            info.id = destinationIdForEndpoint(destination, index);
            info.name = stringFromMidiObjectProperty(destination, kMIDIPropertyDisplayName);
            if (info.name.empty()) {
                info.name = stringFromMidiObjectProperty(destination, kMIDIPropertyName);
            }
            if (info.name.empty()) {
                info.name = "MIDI Output";
            }
            outputs.push_back(info);
        }
        return outputs;
    }

    bool start(const std::string& preferredInputId) {
        std::vector<RecordedMidiEvent> ignoredEvents;
        std::string ignoredError;
        stop(ignoredEvents, ignoredError);

        preferredInputId_ = preferredInputId;
        // notifyProc fires on every CoreMIDI setup change — a device plugged or unplugged, and
        // crucially the re-enumeration a USB MIDI interface goes through when the Mac wakes from
        // sleep. Without it a connection that died overnight stays dead: the endpoint is still
        // listed so nothing looks wrong, but no notes arrive until the app is relaunched.
        const OSStatus clientStatus = MIDIClientCreate(CFSTR("Neuracoust DAW MIDI Input"),
                                                       &Impl::notifyProc, this, &client_);
        if (clientStatus != noErr || client_ == 0) {
            setStoppedStatus("Could not create CoreMIDI client.", 0.0);
            return false;
        }
        const OSStatus portStatus = MIDIInputPortCreate(client_, CFSTR("Neuracoust DAW MIDI Recorder"), &Impl::readProc, this, &port_);
        if (portStatus != noErr || port_ == 0) {
            cleanupCoreMidi();
            setStoppedStatus("Could not create CoreMIDI input port.", 0.0);
            return false;
        }

        const size_t connected = connectPreferredSources();
        if (connected == 0) {
            cleanupCoreMidi();
            setStoppedStatus("No CoreMIDI input source is available.", 0.0);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.clear();
            pendingEvents_.clear();
            recording_ = true;
            startSteady_ = std::chrono::steady_clock::now();
            firstHostSeconds_ = 0.0;
            status_ = {};
            status_.recording = true;
            status_.sourceCount = connected;
            status_.message = connected == 1 ? "Recording MIDI input." : "Recording all MIDI inputs.";
        }
        return true;
    }

    bool stop(std::vector<RecordedMidiEvent>& events, std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events = events_;
            status_.recording = false;
            status_.eventCount = events_.size();
            status_.durationSeconds = elapsedSecondsLocked();
            status_.message = events_.empty() ? "MIDI recording stopped without captured events." : "MIDI recording saved.";
            error = status_.message;
            recording_ = false;
        }
        cleanupCoreMidi();
        return true;
    }

    MidiInputRecordingStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy = status_;
        if (recording_) {
            copy.durationSeconds = elapsedSecondsLocked();
            copy.eventCount = events_.size();
        }
        return copy;
    }

    std::vector<RecordedMidiEvent> consumePendingEvents() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto events = pendingEvents_;
        pendingEvents_.clear();
        return events;
    }

private:
    static void readProc(const MIDIPacketList* packetList, void* readProcRefCon, void*) {
        auto* self = static_cast<Impl*>(readProcRefCon);
        if (self != nullptr) {
            self->handlePackets(packetList);
        }
    }

    double elapsedSecondsLocked() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startSteady_).count();
    }

    double eventSecondsForPacketLocked(const MIDIPacket& packet) {
        const double packetHostSeconds = hostTimeToSeconds(packet.timeStamp);
        if (packetHostSeconds > 0.0) {
            if (firstHostSeconds_ <= 0.0) {
                firstHostSeconds_ = packetHostSeconds;
            }
            return std::max(0.0, packetHostSeconds - firstHostSeconds_);
        }
        return elapsedSecondsLocked();
    }

    void handlePackets(const MIDIPacketList* packetList) {
        if (packetList == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
            return;
        }
        const MIDIPacket* packet = &packetList->packet[0];
        for (UInt32 packetIndex = 0; packetIndex < packetList->numPackets; ++packetIndex) {
            parsePacketLocked(*packet);
            packet = MIDIPacketNext(packet);
        }
        status_.eventCount = events_.size();
        status_.durationSeconds = elapsedSecondsLocked();
    }

    void parsePacketLocked(const MIDIPacket& packet) {
        const double eventSeconds = eventSecondsForPacketLocked(packet);
        UInt16 offset = 0;
        while (offset < packet.length) {
            const uint8_t status = packet.data[offset];
            if ((status & 0x80) == 0) {
                ++offset;
                continue;
            }
            const uint8_t type = status & 0xF0;
            const int channel = static_cast<int>(status & 0x0F) + 1;
            if ((type == 0x80 || type == 0x90 || type == 0xB0 || type == 0xE0) && offset + 2 < packet.length) {
                const int data1 = packet.data[offset + 1];
                const int data2 = packet.data[offset + 2];
                if (type == 0x80 || type == 0x90) {
                    RecordedMidiEvent event;
                    event.kind = (type == 0x80 || data2 == 0) ? RecordedMidiEventKind::NoteOff : RecordedMidiEventKind::NoteOn;
                    event.timeSeconds = eventSeconds;
                    event.channel = channel;
                    event.pitch = data1;
                    event.velocity = data2;
                    events_.push_back(event);
                    pendingEvents_.push_back(event);
                } else if (type == 0xB0) {
                    RecordedMidiEvent event;
                    event.kind = RecordedMidiEventKind::Controller;
                    event.timeSeconds = eventSeconds;
                    event.channel = channel;
                    event.controller = data1;
                    event.value = data2;
                    events_.push_back(event);
                    pendingEvents_.push_back(event);
                } else if (type == 0xE0) {
                    RecordedMidiEvent event;
                    event.kind = RecordedMidiEventKind::PitchBend;
                    event.timeSeconds = eventSeconds;
                    event.channel = channel;
                    event.value = std::max(0, std::min(16383, data1 | (data2 << 7)));
                    events_.push_back(event);
                    pendingEvents_.push_back(event);
                }
                offset += 3;
                continue;
            }
            if (type == 0xC0 && offset + 1 < packet.length) {
                RecordedMidiEvent event;
                event.kind = RecordedMidiEventKind::ProgramChange;
                event.timeSeconds = eventSeconds;
                event.channel = channel;
                event.program = packet.data[offset + 1];
                events_.push_back(event);
                pendingEvents_.push_back(event);
                offset += 2;
                continue;
            }
            offset += 1;
        }
    }

    void setStoppedStatus(const std::string& message, double durationSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.recording = false;
        status_.durationSeconds = durationSeconds;
        status_.sourceCount = 0;
        status_.message = message;
        recording_ = false;
    }

    /// (Re)connect the port to the preferred source, or to every source when no preference is
    /// set. Disconnects whatever it held first, so it is safe to call on a live port to recover
    /// from a wake-from-sleep. Returns how many sources ended up connected. Runs under
    /// connectionMutex_ so a setup-changed notification cannot race a start/stop.
    size_t connectPreferredSources() {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        if (port_ == 0) {
            return 0;
        }
        for (MIDIEndpointRef source : connectedSources_) {
            MIDIPortDisconnectSource(port_, source);
        }
        connectedSources_.clear();

        const ItemCount count = MIDIGetNumberOfSources();
        for (ItemCount index = 0; index < count; ++index) {
            MIDIEndpointRef source = MIDIGetSource(index);
            if (source == 0) {
                continue;
            }
            const std::string sourceId = sourceIdForEndpoint(source, index);
            if (!preferredInputId_.empty() && sourceId != preferredInputId_) {
                continue;
            }
            if (MIDIPortConnectSource(port_, source, nullptr) == noErr) {
                connectedSources_.push_back(source);
            }
        }
        return connectedSources_.size();
    }

    /// CoreMIDI setup changed — a device came or went, or the bus re-enumerated on wake. Re-run
    /// the connection so the keyboard keeps working without a relaunch. Only a real change to the
    /// set of sources matters; ignore the property-changed chatter CoreMIDI also sends.
    static void notifyProc(const MIDINotification* message, void* context) {
        if (message == nullptr || context == nullptr) {
            return;
        }
        if (message->messageID != kMIDIMsgSetupChanged &&
            message->messageID != kMIDIMsgObjectAdded &&
            message->messageID != kMIDIMsgObjectRemoved) {
            return;
        }
        auto* impl = static_cast<Impl*>(context);
        if (!impl->recording_) {
            return;
        }
        const size_t connected = impl->connectPreferredSources();
        std::lock_guard<std::mutex> lock(impl->mutex_);
        impl->status_.sourceCount = connected;
    }

    void cleanupCoreMidi() {
        // Take the connection lock, detach everything, then dispose OUTSIDE the lock: a
        // setup-changed notification can fire during MIDIClientDispose, and its notifyProc also
        // wants connectionMutex_ — holding it across dispose could deadlock. Nulling the members
        // first makes any in-flight notify a no-op.
        MIDIPortRef port = 0;
        MIDIClientRef client = 0;
        std::vector<MIDIEndpointRef> sources;
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            port = port_;
            client = client_;
            sources = std::move(connectedSources_);
            connectedSources_.clear();
            port_ = 0;
            client_ = 0;
        }
        if (port != 0) {
            for (MIDIEndpointRef source : sources) {
                MIDIPortDisconnectSource(port, source);
            }
            MIDIPortDispose(port);
        }
        if (client != 0) {
            MIDIClientDispose(client);
        }
    }

    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    std::vector<MIDIEndpointRef> connectedSources_;
    std::string preferredInputId_;
    // Guards the CoreMIDI connection (client_/port_/connectedSources_/preferredInputId_) so the
    // setup-changed notifyProc, which runs on CoreMIDI's thread, cannot race start/stop.
    std::mutex connectionMutex_;
    std::vector<RecordedMidiEvent> events_;
    std::vector<RecordedMidiEvent> pendingEvents_;
    std::chrono::steady_clock::time_point startSteady_ = std::chrono::steady_clock::now();
    double firstHostSeconds_ = 0.0;
    bool recording_ = false;
    mutable std::mutex mutex_;
    MidiInputRecordingStatus status_;
};

MidiInputRecorder::MidiInputRecorder() : impl_(std::make_unique<Impl>()) {}
MidiInputRecorder::~MidiInputRecorder() = default;
std::vector<MidiInputSourceInfo> MidiInputRecorder::availableInputs() const { return impl_->availableInputs(); }
std::vector<MidiInputSourceInfo> MidiInputRecorder::availableOutputs() const { return impl_->availableOutputs(); }
bool MidiInputRecorder::start(const std::string& preferredInputId) { return impl_->start(preferredInputId); }
bool MidiInputRecorder::stop(std::vector<RecordedMidiEvent>& events, std::string& error) { return impl_->stop(events, error); }
std::vector<RecordedMidiEvent> MidiInputRecorder::consumePendingEvents() { return impl_->consumePendingEvents(); }
MidiInputRecordingStatus MidiInputRecorder::status() const { return impl_->status(); }

} // namespace neuracoust::daw

#endif
