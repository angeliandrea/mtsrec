#!/usr/bin/env bash
# ffmpeg compilato dal sorgente per stare DENTRO mtsrec.exe (sezione 9.81).
#
# Perché: Smart App Control di Windows 11 blocca le DLL non firmate, e SignPath firma solo i binari
# costruiti dal nostro sorgente. Quindi ffmpeg non viaggia più come DLL di BtbN: si compila qui, in
# librerie statiche, e finisce nell'eseguibile firmato.
#
# Perché clang-cl: produce librerie nel formato del compilatore Microsoft, con cui si collega il
# registratore, e accetta l'assembler in linea di ffmpeg che il compilatore Microsoft rifiuta. Senza
# quello il DNxHD codifica 3,5 volte più piano (sezione 9.54).
#
# Solo i pezzi che il registratore usa: codificatori DNxHD/DNxHR, MJPEG per le finestrelle, PCM;
# contenitori MOV e WAV in scrittura e in lettura (riparazione e --sonda); il protocollo file.
# Licenza: LGPL 2.1 o successiva, senza --enable-gpl né --enable-nonfree.
#
# Si lancia da compila-ffmpeg.bat, che prepara l'ambiente di Visual Studio e MSYS2:
#   compila-ffmpeg.sh SORGENTE DESTINAZIONE
set -euo pipefail
SRC="$1"
OUT="$2"

# link.exe deve essere quello di Microsoft, non il «link» di MSYS2 che sta in /usr/bin.
MSVC_BIN="$(dirname "$(command -v cl.exe)")"
export PATH="$MSVC_BIN:/clang64/bin:$PATH"

BUILD="$OUT/build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

"$SRC/configure" \
    --prefix="$OUT" \
    --toolchain=msvc --cc=clang-cl --cxx=clang-cl \
    --target-os=win64 --arch=x86_64 \
    --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-network --disable-autodetect --disable-debug \
    --disable-everything \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-encoder=dnxhd,mjpeg,pcm_s16le,pcm_s24le \
    --enable-decoder=dnxhd,pcm_s16le,pcm_s24le \
    --enable-muxer=mov,wav \
    --enable-demuxer=mov,wav \
    --enable-protocol=file \
    --enable-x86asm --x86asmexe=nasm \
    --extra-cflags="-MT -O2" \
    --pkg-config-flags=--static

# Il controllo che conta: senza assembler in linea il DNxHD torna lento.
grep -E '^#define (HAVE_INLINE_ASM|HAVE_X86ASM|HAVE_MMX_INLINE|HAVE_SSE2_INLINE) ' config.h

make -j"$(nproc)"
make install
