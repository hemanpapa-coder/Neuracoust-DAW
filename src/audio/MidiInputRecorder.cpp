#include "audio/MidiInputRecorder.h"

#if !defined(__APPLE__)
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace neuracoust::daw {

#if defined(_WIN32)

class MidiInputRecorder::Impl {
public:
    ~Impl() {
        std::vector<RecordedMidiEvent> ignoredEvents;
        std::string ignoredError;
        stop(ignoredEvents, ignoredError);
    }

    std::vector<MidiInputSourceInfo> availableInputs() const {
        std::vector<MidiInputSourceInfo> inputs;
        const UINT count = midiInGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            MIDIINCAPSA caps {};
            if (midiInGetDevCapsA(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
                continue;
            }
            MidiInputSourceInfo info;
            info.id = std::to_string(index);
            info.name = caps.szPname[0] != '\0' ? caps.szPname : "MIDI Input";
            inputs.push_back(info);
        }
        return inputs;
    }

    std::vector<MidiInputSourceInfo> availableOutputs() const {
        std::vector<MidiInputSourceInfo> outputs;
        const UINT count = midiOutGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            MIDIOUTCAPSA caps {};
            if (midiOutGetDevCapsA(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
                continue;
            }
            MidiInputSourceInfo info;
            info.id = std::to_string(index);
            info.name = caps.szPname[0] != '\0' ? caps.szPname : "MIDI Output";
            outputs.push_back(info);
        }
        return outputs;
    }

    bool start(const std::string& preferredInputId) {
        std::vector<RecordedMidiEvent> ignoredEvents;
        std::string ignoredError;
        stop(ignoredEvents, ignoredError);

        const UINT count = midiInGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            if (!preferredInputId.empty() && preferredInputId != std::to_string(index)) {
                continue;
            }
            HMIDIIN handle = nullptr;
            if (midiInOpen(&handle, index, reinterpret_cast<DWORD_PTR>(&Impl::midiInProc), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR ||
                handle == nullptr) {
                continue;
            }
            if (midiInStart(handle) != MMSYSERR_NOERROR) {
                midiInClose(handle);
                continue;
            }
            handles_.push_back(handle);
        }

        if (handles_.empty()) {
            setStoppedStatus("No Windows MIDI input source is available.");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.clear();
            pendingEvents_.clear();
            startSteady_ = std::chrono::steady_clock::now();
            recording_ = true;
            status_ = {};
            status_.recording = true;
            status_.sourceCount = handles_.size();
            status_.message = handles_.size() == 1 ? "Recording Windows MIDI input." : "Recording all Windows MIDI inputs.";
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
        for (HMIDIIN handle : handles_) {
            midiInStop(handle);
            midiInReset(handle);
            midiInClose(handle);
        }
        handles_.clear();
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
    static void CALLBACK midiInProc(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (message != MIM_DATA || instance == 0) {
            return;
        }
        auto* self = reinterpret_cast<Impl*>(instance);
        self->handleShortMessage(static_cast<DWORD>(param1));
    }

    double elapsedSecondsLocked() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startSteady_).count();
    }

    void handleShortMessage(DWORD data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
            return;
        }
        const uint8_t statusByte = static_cast<uint8_t>(data & 0xFF);
        if ((statusByte & 0x80) == 0) {
            return;
        }
        const uint8_t type = statusByte & 0xF0;
        const int channel = static_cast<int>(statusByte & 0x0F) + 1;
        const int data1 = static_cast<int>((data >> 8) & 0x7F);
        const int data2 = static_cast<int>((data >> 16) & 0x7F);
        RecordedMidiEvent event;
        event.timeSeconds = elapsedSecondsLocked();
        event.channel = channel;
        if (type == 0x80 || type == 0x90) {
            event.kind = (type == 0x80 || data2 == 0) ? RecordedMidiEventKind::NoteOff : RecordedMidiEventKind::NoteOn;
            event.pitch = data1;
            event.velocity = data2;
        } else if (type == 0xB0) {
            event.kind = RecordedMidiEventKind::Controller;
            event.controller = data1;
            event.value = data2;
        } else if (type == 0xC0) {
            event.kind = RecordedMidiEventKind::ProgramChange;
            event.program = data1;
        } else if (type == 0xE0) {
            event.kind = RecordedMidiEventKind::PitchBend;
            event.value = (std::max)(0, (std::min)(16383, data1 | (data2 << 7)));
        } else {
            return;
        }
        events_.push_back(event);
        pendingEvents_.push_back(event);
        status_.eventCount = events_.size();
        status_.durationSeconds = elapsedSecondsLocked();
    }

    void setStoppedStatus(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.recording = false;
        status_.sourceCount = 0;
        status_.message = message;
        recording_ = false;
    }

private:
    std::vector<HMIDIIN> handles_;
    std::vector<RecordedMidiEvent> events_;
    std::vector<RecordedMidiEvent> pendingEvents_;
    std::chrono::steady_clock::time_point startSteady_ = std::chrono::steady_clock::now();
    mutable std::mutex mutex_;
    bool recording_ = false;
    MidiInputRecordingStatus status_;
};

#else

class MidiInputRecorder::Impl {
public:
    std::vector<MidiInputSourceInfo> availableInputs() const {
        std::vector<MidiInputSourceInfo> inputs;
        std::ifstream clients("/proc/asound/seq/clients");
        std::string line;
        std::string currentClient;
        while (std::getline(clients, line)) {
            const auto clientPos = line.find("Client ");
            const auto namePos = line.find("'");
            if (clientPos != std::string::npos && namePos != std::string::npos) {
                const auto colonPos = line.find(':', clientPos);
                currentClient = colonPos == std::string::npos
                    ? std::string()
                    : line.substr(clientPos + 7, colonPos - (clientPos + 7));
                const auto endNamePos = line.find("'", namePos + 1);
                MidiInputSourceInfo info;
                info.id = currentClient;
                info.name = endNamePos == std::string::npos ? line.substr(namePos + 1) : line.substr(namePos + 1, endNamePos - namePos - 1);
                if (!info.id.empty() && !info.name.empty() && info.name != "System") {
                    inputs.push_back(info);
                }
            }
        }
        return inputs;
    }

    std::vector<MidiInputSourceInfo> availableOutputs() const {
        return {};
    }

    bool start(const std::string&) {
        status_.recording = false;
        status_.message = "ALSA MIDI input capture is not built into this Linux DAW binary yet.";
        return false;
    }

    bool stop(std::vector<RecordedMidiEvent>& events, std::string& error) {
        events.clear();
        error = status_.message;
        status_.recording = false;
        return true;
    }

    std::vector<RecordedMidiEvent> consumePendingEvents() {
        return {};
    }

    MidiInputRecordingStatus status() const {
        return status_;
    }

private:
    MidiInputRecordingStatus status_;
};

#endif

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
