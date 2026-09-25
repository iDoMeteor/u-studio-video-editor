# Records SDL's disk-audio FIFO into a wall-clock-aligned raw file. SDL's
# disk driver paces output in real time but truncates/reopens its file on
# every device open (the app restarts the consumer on each edit), so each
# reopen is placed at its wall-clock position with silence filling gaps.
import os, sys, time
fifo, out, rate, ch, bps = sys.argv[1], sys.argv[2], 48000, 2, 2
frame = ch * bps
t0 = float(sys.argv[3]) if len(sys.argv) > 3 else time.time()
with open(out, 'wb') as f:
    pos = 0
    while True:
        try:
            fd = os.open(fifo, os.O_RDONLY)
        except FileNotFoundError:
            break
        # A new writer: pad silence up to the current wall-clock position.
        want = int((time.time() - t0) * rate) * frame
        if want > pos:
            f.write(b'\0' * (want - pos)); pos = want
        while True:
            data = os.read(fd, 65536)
            if not data:
                break
            f.write(data); pos += len(data)
        os.close(fd)
        f.flush()
        if os.path.exists(fifo + '.stop'):
            break
