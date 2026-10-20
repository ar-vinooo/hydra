# Peta cakupan instruksi dan fault yang terlacak (20 Oktober 2026)

Status: VERIFIED = hasil dibandingkan dengan model Python independen (`tests/maxwell_*_reference.py`,
`bash tests/run_python_references.sh`); IMPLEMENTED_UNVERIFIED = kode ada, belum ada pembanding independen;
PARTIAL = sebagian mode; MISSING = tidak ada. Semua bukti adalah Python vs Apple GPU, bukan eksekusi NVIDIA.

## Fault yang terlacak

Fitur yang dilewati decoder/analyzer/emitter tidak lagi hanya masuk log. `SHADER_UNSUPPORTED(...)`
(`shader_decompiler/unsupported.hpp`) menulis log seperti `LOG_NOT_IMPLEMENTED` **dan** mencatat teks fitur ke
`UnsupportedCollector` milik `decompile()`. Hasilnya ada di `ResourceMapping::unsupported`; `ShaderCache`
mencatat satu baris per shader: `Shader <gpu_addr> (<tipe>) uses unsupported features and may compute wrong
values: ...`. Opcode yang belum didekode muncul sebagai `Opcode <nama>`. Kebijakan: catat lalu lanjut (shader
tetap dibuat, tidak ada game yang berhenti render). Tes: `testUnsupportedFeatureReport` (shader-library) dan
pemeriksaan di `run_python_references.sh` yang gagal bila `LOG_NOT_IMPLEMENTED(ShaderDecompiler` kembali dipakai
langsung di `shader_decompiler/`.

## Peta keluarga

| Keluarga | Status | Bukti | Fault id (teks yang dicatat) |
|---|---|---|---|
| FADD/FMUL/FFMA/FMNMX (+SAT/FTZ/scale) | VERIFIED (denormal = GAP host) | maxwell-alu | - |
| MUFU, RRO | VERIFIED (toleransi ulp) | maxwell-alu | - |
| FSETP/ISETP, FSET/ISET, SEL | VERIFIED | maxwell-alu, predicate (499.296) | - |
| IADD/IADD32I `.CC` `.X` (tanpa negasi) | VERIFIED (baru) | `iadd_cc*`, `iadd_chain64` | - |
| IADD `.CC`/`.X` dengan operand dinegasi (pengurangan 64-bit) | VERIFIED terhadap identitas aritmetika (diperbaiki) | `iadd_neg*_cc`, `iadd_sub64`, `iadd_sub_carry` | - |
| LEA.LO/LEA.HI `.CC` `.X`, ISCADD `.CC` | VERIFIED (baru) | `carry_*` | `LEA predicate output`, `LEA.HI predicate output`, `ISCADD.PO condition code` |
| IADD3 (half, shift, negasi) | VERIFIED | maxwell-alu | `IADD3 X/CC` |
| XMAD (semua mode C, PSL, MRG) | VERIFIED; `.X/.CC` IMPLEMENTED_UNVERIFIED | maxwell-alu | - |
| LOP/LOP3, SHL/SHR, IMNMX, POPC, FLO, BFE/BFI | VERIFIED (kasus tepi BFE/BFI, BREV tidak) | maxwell-alu | `SHL/SHR extended mode/condition code`, `BFI condition code output` |
| SHF | VERIFIED (tes C++ tulisan tangan) | shader-library | `SHF condition code/extended mode` |
| HADD2/HMUL2/HFMA2 | VERIFIED | maxwell-alu | `HSETP2 cbuf |b|` |
| F2F/F2I/I2F/I2I | VERIFIED terhadap model host, bukan hardware | maxwell-conversion/f2f | `F2F CC`, `F2I CC`, `I2F CC`, `64-bit I2I` |
| FP64 (DADD/DMUL/DFMA/DSET/DSETP/DMNMX) | VERIFIED vs host | shader-library | `<op>.CC` |
| ICMP/FCMP, PSETP/PSET, R2P/P2R, FSWZADD, VOTE, SHFL | IMPLEMENTED_UNVERIFIED | tes shader-library sebagian | `Opcode vote` |
| Video (VMAD/VMNMX/VSETP) | PARTIAL | shader-video | `VMAD CC/scale/saturation`, `VMNMX ...`, `VSETP ...`; `Opcode vadd/vshl/vshr/vabsdiff/vset` |
| Texture (TEX/TEXS/TLD/TLDS/TLD4/TXQ/TMML/TXD) | PARTIAL (TEXS, TLDS, TLD4S, tipe, sampler VERIFIED) | texture refs | `Texture query`, `TLDS multisample fetch`, `TMML texture dimension`, `TXD texture dimension/offset` |
| Memori (LDC, LDG/STG/ATOM/RED) | PARTIAL | shader-library | `LDC address mode`, `Global load/store/atomic without a storage buffer`, `Global ATOM.CAS` |
| Shared/surface (LDS/STS/ATOMS, SULD/SUST) | PARTIAL | shader-library | `ATOMS ...`, `Surface type`, `Image type`, `Image atomic op` |
| Alur kontrol (BRA/CAL/RET/PEXIT/JMP/SSY/PBK/CONT) | VERIFIED untuk bentuk biasa | shader-library | `BRX/JMX unbounded...`, `JMP unresolved cbuf target`, `Flow test`, `Opcode pcnt/pbk/ssy/cal/bra/...` |
| Lainnya (OUT, R2B, B2R, PIXLD, CCTL, IMAD/IMADSP, IDP, LEPC, RAM, SAM, RTT...) | MISSING | - | `Opcode <nama>` |

## Catatan terbuka

- **IADD dengan operand dinegasi (diperbaiki).** Dulu `a + (-b) + C`; kata tinggi pengurangan 64-bit meleset 1 dan
  carry untuk b=0 salah. Kini operand dinegasi di-bit-NOT dengan carry-in 1 (atau flag C pada `.X`). Dasarnya
  identitas aritmetika (hanya model ini yang membuat `IADD.CC` + `IADD.X` mengurangkan 64-bit dengan benar,
  C = tidak ada pinjaman), bukan trace hardware atau teks NVIDIA. Tes shader-library lama (`testIntegerConditions`)
  mengkodekan model `-b` dan diubah ke 33-bit; kontrol: decoder lama gagal 4 grup baru (mis. `iadd_sub64` 10/144).
- Hanya flag yang secara definisi aritmetika add yang dibandingkan (Z/S/C/O pada IADD, C pada LEA/ISCADD).
- Oracle belum mencakup XMAD.X/CC, ICMP/FCMP, I2I (semantik tak terdefinisi di sumber), PSETP/PSET.
