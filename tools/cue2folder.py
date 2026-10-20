# SPDX-License-Identifier: MIT
# -----------------------------------------------------------------------------
#  cue2folder.py  --  CUE + BIN（IMG）の CD イメージを、PC98PLAYER の「フォルダ方式」用に分ける
#
#   python cue2folder.py GAME.CUE 出力フォルダ [--mp3] [--bitrate 192k]
#
#   出力フォルダの中にできるもの:
#     TRACK01.ISO        … データトラック（2048 バイト/セクタの ISO。7-Zip などで中身を取り出せる。
#                          そのまま CDROM=TRACK01.ISO としても使える）
#     BGM\track02.wav …  … 音楽トラック（--mp3 なら track02.mp3 …。MP3 にするには ffmpeg が要る）
#
#   PC98PLAYER.INI には CDROM=（ISO か、ISO の中身を取り出したフォルダ）と CDDA=BGM を書く。
#   音楽トラックの長さは INDEX 01 から次のトラックの INDEX 01 まで（PREGAP の無音は含めない）。
# -----------------------------------------------------------------------------
import os, re, shutil, struct, subprocess, sys

def parse_cue(path):
    base = os.path.dirname(os.path.abspath(path))
    raw = open(path, 'rb').read()
    try: text = raw.decode('utf-8')
    except UnicodeDecodeError: text = raw.decode('cp932', 'replace')
    files, cur = [], None
    for line in text.splitlines():
        w = line.strip()
        m = re.match(r'FILE\s+"(.*)"\s+(\S+)', w, re.I) or re.match(r'FILE\s+(\S+)\s+(\S+)', w, re.I)
        if m:
            name = m.group(1)
            p = os.path.join(base, name)
            if not os.path.exists(p):   # 大文字・小文字の違い
                for f in os.listdir(base):
                    if f.lower() == os.path.basename(name).lower(): p = os.path.join(base, f)
            cur = {'path': p, 'tracks': []}; files.append(cur); continue
        m = re.match(r'TRACK\s+(\d+)\s+(\S+)', w, re.I)
        if m and cur is not None:
            cur['tracks'].append({'no': int(m.group(1)), 'mode': m.group(2).upper(), 'idx1': None}); continue
        m = re.match(r'INDEX\s+01\s+(\d+):(\d+):(\d+)', w, re.I)
        if m and cur and cur['tracks']:
            mm, ss, ff = map(int, m.groups())
            cur['tracks'][-1]['idx1'] = (mm * 60 + ss) * 75 + ff
    return files

def sector_size(mode):
    if mode == 'AUDIO': return 2352
    m = re.search(r'/(\d+)', mode)
    return int(m.group(1)) if m else 2352

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    mp3 = '--mp3' in sys.argv
    br = '192k'
    if '--bitrate' in sys.argv: br = sys.argv[sys.argv.index('--bitrate') + 1]; args.remove(br)
    if len(args) < 2:
        print(__doc__ if __doc__ else 'python cue2folder.py GAME.CUE 出力フォルダ [--mp3]'); sys.exit(1)
    cue, out = args[0], args[1]
    ff = shutil.which('ffmpeg') if mp3 else None
    if mp3 and not ff: print('ffmpeg が見つからないので WAV で書きます'); mp3 = False
    os.makedirs(os.path.join(out, 'BGM'), exist_ok=True)
    for f in parse_cue(cue):
        size = os.path.getsize(f['path'])
        tr = f['tracks']
        for i, t in enumerate(tr):
            ss = sector_size(t['mode'])
            start = t['idx1'] * ss
            end = tr[i + 1]['idx1'] * ss if i + 1 < len(tr) else size
            with open(f['path'], 'rb') as fh:
                fh.seek(start)
                if t['mode'] == 'AUDIO':
                    n = end - start
                    data = fh.read(n)
                    name = os.path.join(out, 'BGM', 'track%02d' % t['no'])
                    if mp3:
                        subprocess.run([ff, '-loglevel', 'error', '-y', '-f', 's16le', '-ar', '44100', '-ac', '2', '-i', '-',
                                        '-b:a', br, name + '.mp3'], input=data, check=True)
                        print('トラック %2d → %s.mp3（%d 秒）' % (t['no'], name, n // 176400))
                    else:
                        with open(name + '.wav', 'wb') as o:
                            o.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                                    struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16) + b'data' + struct.pack('<I', len(data)))
                            o.write(data)
                        print('トラック %2d → %s.wav（%d 秒）' % (t['no'], name, n // 176400))
                else:
                    off = {2048: 0, 2336: 8}.get(ss, 24 if 'MODE2' in t['mode'] else 16)
                    name = os.path.join(out, 'TRACK%02d.ISO' % t['no'])
                    n = 0
                    with open(name, 'wb') as o:
                        while fh.tell() + ss <= end:
                            o.write(fh.read(ss)[off:off + 2048]); n += 1
                    print('トラック %2d → %s（データ %d セクタ）' % (t['no'], name, n))

if __name__ == '__main__':
    main()
