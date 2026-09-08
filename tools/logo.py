r"""Draws images/logo.png.

    uv run --quiet --with pillow python .\logo.py

Kept for the fun of it, and because a logo nobody can regenerate is a logo nobody can
change. It will not run on a fresh clone without editing MPQ below: it reads two assets
straight out of an extracted client, and neither is redistributed here.

  * FRIZQT__.TTF, the client's own UI face. Only rasterised glyphs reach the PNG.
  * Dalaran_Sign_Bank.blp, the Dalaran bank doodad sign. Pillow decodes BLP2 directly, so no
    external converter is needed. It arrives as RGB with no alpha, and the coin medallion
    shares its 256x256 sheet with a sun, a bar and a border strip, so it is cut out by circle
    -- the centre and radius below were measured off a gridded overlay rather than guessed.

S is a supersampling factor: the canvas is drawn S times oversize so the plate corners and
the cut circle come out smooth, since there is no vector tooling on the machine this was
written on. The final line decides what ships. Downsampling by S gives a 500x200 asset;
leaving it at full size, as it does now, ships 2x for a crisp 500px display width.
"""

from PIL import Image, ImageDraw, ImageEnhance, ImageFont

MPQ = r"C:\Projects\AoWoW\setup\mpqdata\enUS"
FONT = MPQ + r"\Fonts\FRIZQT__.TTF"
SIGN = MPQ + r"\World\Expansion02\Doodads\Dalaran\Dalaran_Sign_Bank.blp"
OUT = r"C:\Games\AzerothCore\modules\mod-extended-bank\images\logo.png"

S = 2                      # supersampling factor
W, H = 500 * S, 200 * S

PLAQUE      = (26, 29, 36)
PLAQUE_EDGE = (94, 74, 38)
GOLD        = (224, 168, 62)
GOLD_DIM    = (150, 111, 42)
CREAM       = (238, 226, 202)
MUTED       = (152, 145, 130)
PLATE_FACE  = (46, 51, 62)

# How the stack recedes. FADE_BACK is the opacity of the furthest plate, as a fraction; the
# front plate is always fully opaque. FADE_CURVE bends the ramp between them: 1.0 spreads the
# fade evenly, above 1.0 holds the back plates dim for longer so the front two or three carry
# the mark, below 1.0 brightens them early and reads flatter.
FADE_BACK = 0.01
FADE_CURVE = 2.2

# Circle of the medallion within the 256x256 texture, measured off a gridded overlay: the
# sheet also carries a sun, a bar and a border strip that must not come along.
SIGN_CX, SIGN_CY, SIGN_R = 112, 144, 80

img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# --- plaque -----------------------------------------------------------------
# Its own background rather than transparency: the logo has to read on both the light and
# the dark GitHub themes, and gold-on-nothing only works on one of them.
d.rounded_rectangle([0, 0, W - 1, H - 1], radius=16 * S, fill=PLAQUE)
d.rounded_rectangle([0, 0, W - 1, H - 1], radius=16 * S, outline=PLAQUE_EDGE, width=2 * S)


def medallion(diameter):
    """The bank sign, cut from its sheet into a clean circle with a feathered edge."""
    sign = Image.open(SIGN).convert("RGBA")
    # Mask built at 4x the crop and shrunk, which anti-aliases the cut -- Pillow's ellipse
    # is hard-edged, and a hard circle at this size shows every stair step.
    box = SIGN_R * 2
    mask = Image.new("L", (box * 4, box * 4), 0)
    ImageDraw.Draw(mask).ellipse([2, 2, box * 4 - 3, box * 4 - 3], fill=255)
    mask = mask.resize((box, box), Image.LANCZOS)

    cut = sign.crop((SIGN_CX - SIGN_R, SIGN_CY - SIGN_R, SIGN_CX + SIGN_R, SIGN_CY + SIGN_R))

    # The sign is lit for a doodad standing in daylight, so its brown field goes muddy against
    # a dark plate and the coins stop reading. Lifted just enough to separate them.
    rgb = ImageEnhance.Brightness(cut.convert("RGB")).enhance(1.30)
    rgb = ImageEnhance.Contrast(rgb).enhance(1.18)
    cut = rgb.convert("RGBA")

    cut.putalpha(mask)
    return cut.resize((diameter, diameter), Image.LANCZOS)


# --- the mark: eight vault plates receding into the distance -----------------
# Eight because that is ExtendedBank.MaxVaults out of the box, and because the whole feature
# is "your bank, but there are several of them" -- a single container would say nothing the
# word "bank" does not. The front one carries the game's own bank sign.
cx, cy = 96 * S, 100 * S
pw, ph = 92 * S, 68 * S
step = 7 * S
count = 8

for index in range(count):
    depth = index / (count - 1)                  # 0 = furthest back, 1 = front
    offset = (count - 1) / 2 - index
    dx, dy = offset * step, -offset * step

    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    box = [cx + dx - pw // 2, cy + dy - ph // 2, cx + dx + pw // 2, cy + dy + ph // 2]

    # Fading is done with layer alpha rather than by mixing towards the plaque colour, so the
    # stack still recedes correctly if the plaque is ever re-coloured.
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=6 * S, fill=PLATE_FACE, outline=GOLD, width=2 * S)

    alpha = round(255 * (FADE_BACK + (1 - FADE_BACK) * depth ** FADE_CURVE))
    layer.putalpha(layer.getchannel("A").point(lambda value: value * alpha // 255))
    img.alpha_composite(layer)

# The sign sits on the front plate, so it reads as the face of a bank page rather than as a
# separate badge parked beside the stack.
front_x = cx - ((count - 1) / 2) * step
front_y = cy + ((count - 1) / 2) * step
sign = medallion(54 * S)
img.alpha_composite(sign, (int(front_x - sign.width / 2), int(front_y - sign.height / 2)))

# --- wordmark ---------------------------------------------------------------
title = ImageFont.truetype(FONT, 42 * S)
sub = ImageFont.truetype(FONT, 14 * S)

# Baselines are set for optical balance rather than arithmetic centring: the block runs from
# the cap line of "Extended" to the descenders of the motto, so measuring from the box leaves
# it looking high.
tx = 176 * S
d.text((tx, 74 * S), "Extended", font=title, fill=CREAM, anchor="ls")
d.text((tx, 120 * S), "Bank", font=title, fill=GOLD, anchor="ls")

ry = 136 * S
d.line([(tx, ry), (W - 30 * S, ry)], fill=GOLD_DIM, width=1 * S)

# Friz Quadrata ships no bold weight. A faux one -- the same glyphs drawn with a stroke in
# their own colour -- was tried first and blobbed at this size, so the emphasis is colour
# instead. What survives of that experiment is a couple of pixels of extra advance after the
# word, kept only so this file still reproduces the logo that is committed.
TRACKING = max(1, S // 2)

motto = [("Vanilla-like Banks. ", False), ("Plural.", True), (" For AzerothCore.", False)]
mx, my = tx, ry + 20 * S

for segment, strong in motto:
    if strong:
        d.text((mx, my), segment, font=sub, fill=GOLD, anchor="ls")
    else:
        d.text((mx, my), segment, font=sub, fill=MUTED, anchor="ls")

    # See TRACKING above: this nudge is a leftover, not a requirement.
    mx += d.textlength(segment, font=sub) + (2 * TRACKING if strong else 0)

# Swap these two to ship a 1x asset instead.
# img.resize((W // S, H // S), Image.LANCZOS).save(OUT)
img.resize((W, H), Image.LANCZOS).save(OUT)
print("wrote images/logo.png")
