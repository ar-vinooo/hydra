These are self-generated test vectors, not game assets. Each contains three
32x16 solid red frames. H.264 includes a B-frame to exercise delayed output.
The packet file format is `HYVP`, a little-endian u32 packet count, then repeated
little-endian u32 packet sizes followed by the elementary-stream packet bytes.

Generated with FFmpeg from the `color=c=red:s=32x16:r=3` lavfi source, three frames,
using `libx264 -crf 18 -bf 1 -g 10 -pix_fmt yuv420p` for H.264, and
`libvpx-vp9 -lossless 1 -pix_fmt yuv420p` for VP9. H.264 packet boundaries were
read using ffprobe; VP9 packets were extracted from IVF frame records.
