"""Read the owner's WSC1 mixed-mode CD image and preserve its exact CD timeline.

No original game/image files are written. Music is deliberately excluded from ZIPs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cue', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    text = args.cue.read_text(encoding='utf-8-sig')
    files = re.findall(r'^FILE "([^"]+)" BINARY', text, re.M)
    if len(files) != 1:
        raise SystemExit('Only a single BINARY image is supported')
    image = args.cue.parent / files[0]
    tracks, audio_start, kind, number = {}, None, None, None
    for line in text.splitlines():
        m = re.match(r'\s*TRACK (\d+) (\S+)', line)
        if m:
            number, kind = int(m[1]), m[2]
        m = re.match(r'\s*INDEX (\d+) (\d+):(\d+):(\d+)', line)
        if m:
            index, minute, second, frame = map(int, m.groups())
            sector = (minute * 60 + second) * 75 + frame
            if index == 1:
                tracks[number] = (sector, kind)
            if kind == 'AUDIO' and audio_start is None:
                audio_start = sector
    size = image.stat().st_size
    if size % 2352 or len(tracks) != 16 or tracks.get(1) != (0, 'MODE1/2352'):
        raise SystemExit('Not the identified WSC1 16-track raw image')
    if any(tracks[i][1] != 'AUDIO' for i in range(2, 17)):
        raise SystemExit('Unexpected track types')
    with image.open('rb') as f:
        f.seek(16 * 2352 + 16)
        pvd = f.read(2048)
        if pvd[:7] != b'\x01CD001\x01' or pvd[40:72].rstrip(b' ') != b'Kurogane':
            raise SystemExit('WSC1 volume identifier did not match')
    args.destination.mkdir(parents=True, exist_ok=True)
    out = args.destination / 'cd-audio.wav'
    if out.exists():
        with wave.open(str(out), 'rb') as existing:
            if (existing.getnchannels(), existing.getsampwidth(), existing.getframerate(), existing.getnframes()) != (2, 2, 44100, (size // 2352 - audio_start) * 588):
                raise SystemExit('Existing audio differs; refusing to overwrite')
    digest = hashlib.sha256()
    existing_file = out.exists()
    with image.open('rb') as source, wave.open(str(out), 'rb' if existing_file else 'wb') as dest:
        source.seek(audio_start * 2352)
        if not existing_file:
            dest.setnchannels(2)
            dest.setsampwidth(2)
            dest.setframerate(44100)
        while block := source.read(2352 * 4096):
            digest.update(block)
            if existing_file:
                if dest.readframes(len(block) // 4) != block:
                    raise SystemExit('Existing PCM does not match the source image')
            else:
                dest.writeframesraw(block)
    # MCI MSF uses the physical CD lead-in (+150 frames). The WAV includes
    # track 2's pregap; no guessed two-second shift is applied to the samples.
    lead = 150
    lines = ['[Disc]', 'Tracks=16', f'EndFrame={size // 2352 + lead}',
             f'AudioStartFrame={audio_start + lead}', '[Tracks]']
    lines += [f'{i}={tracks[i][0] + lead}' for i in range(1, 17)]
    (args.destination / 'cd-audio.ini').write_text('\n'.join(lines) + '\n', encoding='ascii')
    wav_hash = hashlib.sha256()
    with out.open('rb') as f:
        while block := f.read(1024 * 1024):
            wav_hash.update(block)
    manifest = {'source_image': str(image), 'source_cue': str(args.cue),
                'volume': 'Kurogane', 'tracks': tracks, 'audio_first_sector': audio_start,
                'image_sectors': size // 2352, 'pcm_sha256': digest.hexdigest(),
                'wav_bytes': out.stat().st_size,
                'wav_sha256': wav_hash.hexdigest()}
    (args.destination / 'extraction.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
