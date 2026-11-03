#!/usr/bin/env python3
"""Upscale a texture dump into a texture pack with Real-ESRGAN.

    python upscale.py DUMP_DIR PACK_DIR [--model realesrgan-x4plus] [--models DIR] [--exe realesrgan-ncnn-vulkan]
                      [--scale 4] [--min-size 8] [--skip-existing]

DUMP_DIR holds the PNGs a port writes with --dump-textures (<hash>_<w>x<h>.png); every one is
upscaled into PACK_DIR under the same name, which the port then loads in place of the original.

Backends:
  --exe PATH    the realesrgan-ncnn-vulkan program (GPU, fast; Windows / Linux / macOS releases at
                https://github.com/xinntao/Real-ESRGAN/releases), run once over a temporary folder
  otherwise     the ncnn Python package on the CPU (pip install ncnn pillow numpy; slow), with the
                models from that same release's models/ folder (--models)

Console textures tile, so each one is padded with a wrapped copy of its opposite edges before
upscaling and cropped afterwards: no seams where the game repeats a texture. Alpha is upscaled
separately (Lanczos); textures whose alpha is only on/off (punch-through cut-outs) stay on/off.

  --keep-colour   keep the original's colours: the upscaler's fine detail on top of the original's
                  broad colour (upscalers trained on photos tend to shift and flatten tints)
  --blend X       mix X (0..1) of the plain (bicubic) enlargement back in, for the original's
                  grain where the upscaler smooths it away (grass, gravel)
"""
from PIL import ImageFilter
import argparse, os, shutil, subprocess, sys, tempfile
import numpy as np
from PIL import Image

PAD = 8   # source pixels of wrapped border


def wrap_pad(a, p):
    return np.pad(a, ((p, p), (p, p), (0, 0)), mode="wrap")


def alpha_up(alpha, w, h):
    img = Image.fromarray(alpha).resize((w, h), Image.LANCZOS)
    out = np.asarray(img)
    vals = np.unique(alpha)
    if len(vals) <= 2:                       # on/off: keep it on/off
        lo, hi = int(vals.min()), int(vals.max())
        out = np.where(out >= (lo + hi) / 2, hi, lo).astype(np.uint8)
    return out


class NcnnCPU:
    def __init__(self, models, model):
        import ncnn
        self.ncnn = ncnn
        self.net = ncnn.Net()
        self.net.opt.use_vulkan_compute = False
        self.net.opt.num_threads = os.cpu_count() or 1
        self.net.load_param(os.path.join(models, model + ".param"))
        self.net.load_model(os.path.join(models, model + ".bin"))

    def run(self, rgb):                      # HxWx3 uint8 -> (sH)x(sW)x3 uint8
        ncnn = self.ncnn
        h, w = rgb.shape[:2]
        m = ncnn.Mat.from_pixels(np.ascontiguousarray(rgb), ncnn.Mat.PixelType.PIXEL_RGB, w, h)
        m.substract_mean_normalize([], [1 / 255.0] * 3)
        ex = self.net.create_extractor()
        ex.input("data", m)
        _, out = ex.extract("output")
        a = np.array(out)                    # 3 x H x W, 0..1
        return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8).transpose(1, 2, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump"); ap.add_argument("pack")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--models", default="models")
    ap.add_argument("--exe")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--min-size", type=int, default=8, help="skip textures smaller than this on either side")
    ap.add_argument("--max-size", type=int, default=1024, help="skip textures larger than this on either side")
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--keep-colour", action="store_true")
    ap.add_argument("--blend", type=float, default=0.0)
    args = ap.parse_args()
    os.makedirs(args.pack, exist_ok=True)

    names = sorted(n for n in os.listdir(args.dump) if n.lower().endswith(".png"))
    todo = []
    for n in names:
        if args.skip_existing and os.path.exists(os.path.join(args.pack, n)):
            continue
        im = Image.open(os.path.join(args.dump, n)).convert("RGBA")
        if min(im.size) < args.min_size or max(im.size) > args.max_size:
            continue
        todo.append((n, np.asarray(im)))
    print(f"{len(todo)} of {len(names)} textures to upscale", flush=True)

    s = args.scale
    padded = {n: wrap_pad(a[:, :, :3], PAD) for n, a in todo}
    ups = {}
    if args.exe:
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
            os.makedirs(src); os.makedirs(dst)
            for n, p in padded.items():
                Image.fromarray(p).save(os.path.join(src, n))
            cmd = [args.exe, "-i", src, "-o", dst, "-n", args.model, "-s", str(s), "-f", "png"]
            if os.path.isdir(args.models):
                cmd += ["-m", args.models]
            subprocess.run(cmd, check=True)
            for n in padded:
                ups[n] = np.asarray(Image.open(os.path.join(dst, n)).convert("RGB"))
    else:
        net = NcnnCPU(args.models, args.model)
        for i, (n, p) in enumerate(padded.items()):
            ups[n] = net.run(p)
            if (i + 1) % 10 == 0 or i + 1 == len(padded):
                print(f"  {i + 1}/{len(padded)}", flush=True)

    for n, a in todo:
        h, w = a.shape[:2]
        u = ups[n]
        f = u.shape[0] // (h + 2 * PAD)
        rgb = u[PAD * f:PAD * f + h * f, PAD * f:PAD * f + w * f]
        if args.keep_colour or args.blend > 0:
            plain = np.asarray(Image.fromarray(wrap_pad(a[:, :, :3], PAD)).resize(
                ((w + 2 * PAD) * f, (h + 2 * PAD) * f), Image.BICUBIC)).astype(np.float32)
            up = u.astype(np.float32)
            if args.keep_colour:     # swap the upscale's broad colour for the original's
                r = 1.5 * f
                blur = lambda x: np.asarray(Image.fromarray(np.clip(x, 0, 255).astype(np.uint8)).filter(
                    ImageFilter.GaussianBlur(r))).astype(np.float32)
                up = up - blur(up) + blur(plain)
            if args.blend > 0:
                up = up * (1 - args.blend) + plain * args.blend
            up = np.clip(up + 0.5, 0, 255).astype(np.uint8)
            rgb = up[PAD * f:PAD * f + h * f, PAD * f:PAD * f + w * f]
        alpha = a[:, :, 3]
        out = np.dstack([rgb, alpha_up(alpha, w * f, h * f)]) if alpha.min() < 255 else rgb
        Image.fromarray(out).save(os.path.join(args.pack, n))
    print(f"wrote {len(todo)} textures to {args.pack}")


if __name__ == "__main__":
    sys.exit(main())
