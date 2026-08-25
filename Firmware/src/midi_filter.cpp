#include "midi_filter.h"

namespace midifilter {

namespace {

/// Portamento Control — die EZ-AG kündigt damit jeden Hammer-On und Pull-Off an.
constexpr uint8_t kPortamentoControl = 84;

/// Vorzeichenlose Differenz: übersteht den Überlauf von micros() ohne Sonderfall.
inline bool within(uint32_t now, uint32_t then, uint32_t window) {
    return static_cast<uint32_t>(now - then) <= window;
}

inline bool bit_test(const uint32_t bits[4], uint8_t note) {
    return (bits[note >> 5] & (1u << (note & 31))) != 0;
}

inline void bit_set(uint32_t bits[4], uint8_t note) {
    bits[note >> 5] |= (1u << (note & 31));
}

inline void bit_clear(uint32_t bits[4], uint8_t note) {
    bits[note >> 5] &= ~(1u << (note & 31));
}

inline bool bits_empty(const uint32_t bits[4]) {
    return (bits[0] | bits[1] | bits[2] | bits[3]) == 0;
}

}  // namespace

// MARK: - Konfiguration

Result Filter::set_config(const Config& config) {
    Result out;
    const bool needs_flush = config.output_channel != config_.output_channel ||
                             config.transpose != config_.transpose ||
                             config.monophonic != config_.monophonic;
    if (needs_flush && has_sounding_) {
        Event event;
        event.channel = config_.output_channel;
        event.kind = MessageKind::NoteOff;
        event.data1 = sounding_.out;
        event.data2 = 0;
        out.add(event);
        has_sounding_ = false;
        stack_count_ = 0;
    }
    config_ = config;
    return out;
}

void Filter::reset() {
    for (uint8_t channel = 0; channel < kChannels; ++channel) {
        strings_[channel] = StringState{};
    }
    stack_count_ = 0;
    has_sounding_ = false;
}

Result Filter::panic() {
    Result out;
    if (has_sounding_) {
        out.add(make(MessageKind::NoteOff, sounding_.out, 0));
    }
    out.add(make(MessageKind::ControlChange, 123, 0));  // All Notes Off
    reset();
    return out;
}

// MARK: - Verarbeitung

Result Filter::process(const Event& event) {
    Result out;

    const uint8_t low = config_.lowest_input_channel < config_.highest_input_channel
                            ? config_.lowest_input_channel
                            : config_.highest_input_channel;
    const uint8_t high = config_.lowest_input_channel < config_.highest_input_channel
                             ? config_.highest_input_channel
                             : config_.lowest_input_channel;
    if (event.channel < low || event.channel > high) {
        if (config_.pass_other_channels) {
            out.add(event);
            out.verdict = Verdict::Passed;
        } else {
            out.verdict = Verdict::FilteredChannel;
        }
        return out;
    }

    switch (event.kind) {
        case MessageKind::NoteOn:
            return note_on(event);
        case MessageKind::NoteOff:
            return note_off(event);
        case MessageKind::ControlChange:
            if (event.data1 == kPortamentoControl) {
                strings_[event.channel].has_portamento = true;
                strings_[event.channel].portamento_us = event.time_us;
                if (config_.suppress_hammer_ons) {
                    // Weiterleiten würde einen Synth mit Portamento zu einer Note
                    // gleiten lassen, die wir gleich wegwerfen.
                    out.verdict = Verdict::SuppressedHammerOn;
                    return out;
                }
            }
            break;
        default:
            break;
    }

    if (!config_.pass_other_messages) {
        out.verdict = Verdict::DroppedOther;
        return out;
    }
    Event forwarded = event;
    forwarded.channel = config_.output_channel;
    out.add(forwarded);
    return out;
}

Result Filter::note_on(const Event& event) {
    Result out;
    StringState& string = strings_[event.channel];
    const uint8_t note = event.data1;
    const bool string_was_silent = bits_empty(string.held);
    bit_set(string.held, note);

    if (config_.suppress_hammer_ons && string.has_portamento &&
        within(event.time_us, string.portamento_us, config_.hammer_on_window_us)) {
        string.has_portamento = false;
        // Gleich darauf beendet das Instrument die Note, die diese hier ersetzt. Auch
        // dieses Note Off muss weg, sonst verstummt der Synth beim Griffwechsel.
        string.legato_guard = true;
        string.legato_guard_until_us = event.time_us + config_.hammer_on_window_us;

        // Die tatsächlich klingende Note niemals markieren — ihr echtes Note Off muss
        // durchkommen, sonst hängt sie für immer.
        const bool is_the_sounding_note =
            has_sounding_ && sounding_.channel == event.channel && sounding_.source == note;
        if (!is_the_sounding_note) {
            bit_set(string.suppressed, note);
        }
        out.verdict = Verdict::SuppressedHammerOn;
        return out;
    }

    if (is_fret_press(event, string_was_silent)) {
        bit_set(string.suppressed, note);
        out.verdict = Verdict::SuppressedFret;
        return out;
    }

    emit_note_on(event.channel, note, event.data2, out);
    return out;
}

Result Filter::note_off(const Event& event) {
    Result out;
    StringState& string = strings_[event.channel];
    const uint8_t note = event.data1;

    bit_clear(string.held, note);
    // Auch für unterdrückte Noten merken: das Note Off einer Bund-Note ist genau die
    // Marke, an der die Retrigger-Erkennung den folgenden Anschlag durchlässt.
    string.last_note_off_note = note;
    string.last_note_off_us = event.time_us;

    if (bit_test(string.suppressed, note)) {
        bit_clear(string.suppressed, note);
        out.verdict = Verdict::SwallowedNoteOff;
        return out;
    }
    // Das Note Off, das zu einem gerade verworfenen Hammer-On gehört. Der Vergleich über
    // die vorzeichenbehaftete Differenz ist auch über den Überlauf hinweg richtig.
    if (string.legato_guard &&
        static_cast<int32_t>(string.legato_guard_until_us - event.time_us) >= 0) {
        string.legato_guard = false;
        out.verdict = Verdict::SwallowedNoteOff;
        return out;
    }

    emit_note_off(event.channel, note, out);
    return out;
}

bool Filter::is_fret_press(const Event& event, bool string_was_silent) const {
    switch (config_.fret_mode) {
        case FretMode::Off:
            return false;
        case FretMode::Velocity:
            return config_.velocity_exact_match ? event.data2 == config_.velocity_threshold
                                                : event.data2 <= config_.velocity_threshold;
        case FretMode::Retrigger: {
            const StringState& string = strings_[event.channel];
            if (string.last_note_off_note == event.data1 &&
                within(event.time_us, string.last_note_off_us, config_.retrigger_window_us)) {
                return false;  // Note Off → Note On: ein Anschlag auf klingende Saite
            }
            return !(config_.allow_first_note_on_idle_string && string_was_silent);
        }
    }
    return false;
}

// MARK: - Monophone Stimmenverwaltung

void Filter::emit_note_on(uint8_t channel, uint8_t note, uint8_t velocity, Result& out) {
    const uint8_t target = transposed(note);
    if (!config_.monophonic) {
        out.add(make(MessageKind::NoteOn, target, velocity));
        return;
    }

    // Denselben Ton derselben Saite nicht doppelt stapeln.
    for (uint8_t i = 0; i < stack_count_; ++i) {
        if (stack_[i].channel == channel && stack_[i].source == note) {
            for (uint8_t j = i; j + 1 < stack_count_; ++j) {
                stack_[j] = stack_[j + 1];
            }
            --stack_count_;
            break;
        }
    }
    if (stack_count_ == kStackSize) {
        for (uint8_t j = 0; j + 1 < stack_count_; ++j) {
            stack_[j] = stack_[j + 1];
        }
        --stack_count_;
    }

    HeldNote attacked;
    attacked.channel = channel;
    attacked.source = note;
    attacked.out = target;
    attacked.velocity = velocity;
    stack_[stack_count_++] = attacked;

    update_voice(&stack_[stack_count_ - 1], out);
}

void Filter::emit_note_off(uint8_t channel, uint8_t note, Result& out) {
    if (!config_.monophonic) {
        out.add(make(MessageKind::NoteOff, transposed(note), 0));
        return;
    }

    const bool was_sounding =
        has_sounding_ && sounding_.channel == channel && sounding_.source == note;

    for (uint8_t i = 0; i < stack_count_; ++i) {
        if (stack_[i].channel == channel && stack_[i].source == note) {
            for (uint8_t j = i; j + 1 < stack_count_; ++j) {
                stack_[j] = stack_[j + 1];
            }
            --stack_count_;
            break;
        }
    }

    if (!config_.fallback_to_held_note) {
        // Es klingt immer nur der zuletzt angeschlagene Ton. Loslassen beendet ihn;
        // noch gegriffene Noten bleiben stumm, bis sie wirklich angeschlagen werden.
        if (was_sounding) {
            out.add(make(MessageKind::NoteOff, sounding_.out, 0));
            has_sounding_ = false;
        }
        return;
    }

    update_voice(nullptr, out);
}

const Filter::HeldNote* Filter::pick_winner(const HeldNote* attacked) const {
    // Ohne Rückfall gibt es nichts zurückzukehren, die Priorität hat also nichts mehr zu
    // entscheiden: der zuletzt angeschlagene Ton bekommt die Stimme. Sonst würde eine
    // gehaltene hohe Note jeden später gespielten tieferen Ton verschlucken.
    if (!config_.fallback_to_held_note && attacked != nullptr) {
        return attacked;
    }
    if (stack_count_ == 0) {
        return nullptr;
    }

    switch (config_.priority) {
        case NotePriority::Last:
            return &stack_[stack_count_ - 1];
        case NotePriority::Highest: {
            const HeldNote* best = &stack_[0];
            for (uint8_t i = 1; i < stack_count_; ++i) {
                if (stack_[i].out > best->out) best = &stack_[i];
            }
            return best;
        }
        case NotePriority::Lowest: {
            const HeldNote* best = &stack_[0];
            for (uint8_t i = 1; i < stack_count_; ++i) {
                if (stack_[i].out < best->out) best = &stack_[i];
            }
            return best;
        }
    }
    return nullptr;
}

void Filter::update_voice(const HeldNote* attacked, Result& out) {
    const HeldNote* winner = pick_winner(attacked);

    if (!has_sounding_ && winner == nullptr) {
        return;
    }
    if (has_sounding_ && winner == nullptr) {
        out.add(make(MessageKind::NoteOff, sounding_.out, 0));
        has_sounding_ = false;
        return;
    }
    if (!has_sounding_) {
        out.add(make(MessageKind::NoteOn, winner->out, winner->velocity));
        sounding_ = *winner;
        has_sounding_ = true;
        return;
    }

    if (sounding_.out == winner->out) {
        // Gleiche Tonhöhe. Nur neu anreissen, wenn genau diese Note erneut angeschlagen
        // wurde — nicht, wenn sich bloss eine andere Saite dahinter eingereiht hat.
        const bool struck_again = attacked != nullptr && attacked->channel == winner->channel &&
                                  attacked->source == winner->source;
        if (struck_again && config_.retrigger_same_note) {
            out.add(make(MessageKind::NoteOff, sounding_.out, 0));
            out.add(make(MessageKind::NoteOn, winner->out, winner->velocity));
        }
        sounding_ = *winner;
        return;
    }

    if (config_.legato) {
        // Neue Note zuerst, damit ein Mono-Synth im Legato-Modus gleitet statt seine
        // Hüllkurve neu zu triggern.
        out.add(make(MessageKind::NoteOn, winner->out, winner->velocity));
        out.add(make(MessageKind::NoteOff, sounding_.out, 0));
    } else {
        out.add(make(MessageKind::NoteOff, sounding_.out, 0));
        out.add(make(MessageKind::NoteOn, winner->out, winner->velocity));
    }
    sounding_ = *winner;
}

// MARK: - Hilfsmittel

Event Filter::make(MessageKind kind, uint8_t data1, uint8_t data2) const {
    Event event;
    event.channel = config_.output_channel;
    event.kind = kind;
    event.data1 = data1;
    event.data2 = data2;
    return event;
}

uint8_t Filter::transposed(uint8_t note) const {
    const int value = static_cast<int>(note) + config_.transpose;
    if (value < 0) return 0;
    if (value > 127) return 127;
    return static_cast<uint8_t>(value);
}

}  // namespace midifilter
