# midi_to_buzzer_array.py
# Usage:
#   python midi_to_buzzer_array.py input.mid --track 0 --channel -1 --dpq 24 --name SIGNAL_TUNE_MY_SONG
#
# Requires: pip install mido

import argparse
from mido import MidiFile, merge_tracks
import math

NOTE_NAMES_SHARP = ['C', 'Csharp_Dflat', 'D', 'Dsharp_Eflat', 'E', 'F',
                    'Fsharp_Gflat', 'G', 'Gsharp_Aflat', 'A', 'Asharp_Bflat', 'B']

def note_constant(note_number):
    """Return BUZZER_NOTE_* constant name from MIDI pitch number."""
    # MIDI: A4 = 69, but we only need note name + octave
    octave = (note_number // 12) - 1
    name_idx = note_number % 12
    # Your constants look like BUZZER_NOTE_F6SHARP_G6FLAT etc.
    raw = NOTE_NAMES_SHARP[name_idx]
    # Format to match your style: F6SHARP_G6FLAT (no separators)
    if 'sharp' in raw or 'flat' in raw:
        base = raw.split('_')[0]   # e.g., 'Csharp'
        alt  = raw.split('_')[1]   # e.g., 'Dflat'
        def fmt(s):
            if 'sharp' in s:
                letter = s[0].upper()
                return f"{letter}{octave}SHARP"
            elif 'flat' in s:
                letter = s[0].upper()
                return f"{letter}{octave}FLAT"
            else:
                return f"{s.upper()}{octave}"
        left = fmt(base)
        right = fmt(alt)
        return f"BUZZER_NOTE_{left}_{right}"
    else:
        return f"BUZZER_NOTE_{raw.upper()}{octave}"

def quantize(ticks, ticks_per_beat, dpq):
    """Convert raw MIDI ticks to your firmware units (divisions per quarter)."""
    # ticks  -> beats = ticks / ticks_per_beat
    # beats  -> dpq units = beats * dpq
    units = (ticks / ticks_per_beat) * dpq
    # Round to nearest int, but keep at least 1 if nonzero
    v = int(round(units))
    return max(v, 1) if ticks > 0 else 0

def extract_monophonic_events(mid, use_track=None, use_channel=None):
    """
    Return a list of (type, midi_note_or_None, ticks_duration) where type in {'note','rest'}.
    Strategy: walk events in time order; when a note is on, we hold it until its matching note_off.
    Any gap between notes becomes a rest.
    """
    if use_track is not None:
        tracks = [mid.tracks[use_track]]
    else:
        # merged keeps timing correct across tracks
        tracks = [merge_tracks(mid.tracks)]

    events = []
    for tr in tracks:
        time = 0
        on_notes = {}  # note -> absolute start time (ticks)
        # Build a raw list of (abs_time, msg) and then process
        abs_msgs = []
        t = 0
        for msg in tr:
            t += msg.time
            abs_msgs.append((t, msg))
        # Filter by channel if requested
        abs_msgs = [(t, m) for (t, m) in abs_msgs
                    if not hasattr(m, 'channel') or use_channel is None or m.channel == use_channel]

        # We’ll build a monophonic lane by choosing the earliest-starting active note
        cursor = 0
        current_note = None
        note_start = None

        # Prepare a heap-like stream of note on/offs in time order
        i = 0
        # For rests: track the last time we emitted anything
        last_emit_time = 0

        while i < len(abs_msgs):
            t, m = abs_msgs[i]
            if m.type == 'note_on' and m.velocity > 0:
                # If we are currently not playing a note, any gap since last_emit_time is a rest
                if current_note is None and t > last_emit_time:
                    rest_ticks = t - last_emit_time
                    events.append(('rest', None, rest_ticks))
                    last_emit_time = t
                # If we’re not holding a note, start this one
                if current_note is None:
                    current_note = m.note
                    note_start = t
                # If already holding a note, ignore overlaps (monophonic simplification)
            elif (m.type == 'note_off') or (m.type == 'note_on' and m.velocity == 0):
                # If this ends the note we’re currently holding, emit it
                if current_note is not None and m.note == current_note:
                    dur = t - note_start
                    if dur > 0:
                        events.append(('note', current_note, dur))
                        last_emit_time = t
                    current_note = None
                    note_start = None
                # Else it turns off some overlapped note we ignored; skip
            i += 1

        # If a note was still held to the end, emit it
        if current_note is not None and note_start is not None:
            end_time = abs_msgs[-1][0] if abs_msgs else 0
            dur = max(0, end_time - note_start)
            if dur > 0:
                events.append(('note', current_note, dur))
                last_emit_time = end_time

        # No trailing rest needed

    return events

def main():
    ap = argparse.ArgumentParser(description="Convert MIDI to C int8_t buzzer array.")
    ap.add_argument("midi")
    ap.add_argument("--track", type=int, default=None, help="Track index to use (default: merged)")
    ap.add_argument("--channel", type=int, default=None, help="MIDI channel 0-15 (default: any)")
    ap.add_argument("--dpq", type=int, default=24, help="Divisions per quarter note (firmware units)")
    ap.add_argument("--name", required=True, help="C macro/array name suffix, e.g., SIGNAL_TUNE_MY_SONG")
    ap.add_argument("--indent", type=int, default=4)
    ap.add_argument("--max_rest_join", type=int, default=0,
                    help="Quantized units: join consecutive small rests into neighbors (0=off)")
    args = ap.parse_args()

    mid = MidiFile(args.midi)
    events = extract_monophonic_events(mid, use_track=args.track, use_channel=args.channel)

    # Convert ticks to your units
    items = []
    for kind, note, ticks in events:
        q = quantize(ticks, mid.ticks_per_beat, args.dpq)
        if q == 0:
            continue
        if kind == 'rest':
            items.append(("BUZZER_NOTE_REST", q))
        else:
            items.append((note_constant(note), q))

    # Optional tiny smoothing: merge adjacent rests
    if args.max_rest_join > 0:
        merged = []
        for name, dur in items:
            if merged and name == "BUZZER_NOTE_REST" and merged[-1][0] == "BUZZER_NOTE_REST":
                prev = merged.pop()
                nd = prev[1] + dur
                # re-split if desired, but here we just merge
                merged.append((name, nd))
            else:
                merged.append((name, dur))
        items = merged

    # Emit C
    indent = " " * args.indent
    array_name = f"signal_tune"
    macro_name = args.name
    print(f"#ifdef {macro_name}")
    print("int8_t signal_tune[] = {")
    line = indent
    col = len(line)
    def flush_pair(name, val):
        nonlocal line, col
        chunk = f"{name}, {val}, "
        if col + len(chunk) > 90:
            print(line.rstrip())
            line = indent
            col = len(line)
        line += chunk
        col += len(chunk)

    for name, val in items:
        flush_pair(name, val)

    print(line.rstrip())
    print(f"{indent}0}};")
    print(f"#endif // {macro_name}")

if __name__ == "__main__":
    main()
