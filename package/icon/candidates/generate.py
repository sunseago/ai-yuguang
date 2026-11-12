#!/usr/bin/env python3
"""AI余光 图标候选（Apple 风格）矢量生成器。
参考 macOS Big Sur+ 图标规范：824x824 连续圆角矩形居中、柔和投影、
顶部高光、单一隐喻、不放文字、小尺寸仍可辨。
"""
import math, pathlib
OUT = pathlib.Path(__file__).parent
CX, CY, W, H = 512, 512, 824, 824          # 图标主体占 824，四周留 100
X0, Y0 = CX - W/2, CY - H/2

def squircle(cx, cy, w, h, n=4.6, steps=256):
    """超椭圆（连续圆角）路径，比 rect rx 更接近 Apple 的 squircle。"""
    a, b = w/2, h/2
    pts = []
    for i in range(steps):
        t = 2*math.pi*i/steps
        c, s = math.cos(t), math.sin(t)
        x = a*math.copysign(abs(c)**(2/n), c)
        y = b*math.copysign(abs(s)**(2/n), s)
        pts.append((cx+x, cy+y))
    d = "M " + " L ".join(f"{x:.2f},{y:.2f}" for x, y in pts) + " Z"
    return d

SQ = squircle(CX, CY, W, H)

def defs(bg1, bg2, extra=""):
    return f"""<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1">
    <stop offset="0" stop-color="{bg1}"/><stop offset="1" stop-color="{bg2}"/>
  </linearGradient>
  <linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.22"/>
    <stop offset="0.45" stop-color="#FFFFFF" stop-opacity="0.03"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0.10"/>
  </linearGradient>
  <filter id="shadow" x="-30%" y="-30%" width="160%" height="170%">
    <feDropShadow dx="0" dy="26" stdDeviation="30" flood-color="#000000" flood-opacity="0.35"/>
  </filter>
  <filter id="glow" x="-80%" y="-80%" width="260%" height="260%">
    <feGaussianBlur stdDeviation="18"/>
  </filter>
  {extra}
</defs>"""

def frame(bg1, bg2, inner, extra_defs=""):
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
{defs(bg1,bg2,extra_defs)}
  <g filter="url(#shadow)"><path d="{SQ}" fill="url(#bg)"/></g>
  <path d="{SQ}" fill="url(#gloss)"/>
  <path d="{SQ}" fill="none" stroke="#FFFFFF" stroke-opacity="0.16" stroke-width="3"/>
  <g clip-path="none">{inner}</g>
</svg>"""

# ---------- A 余光·眼：眼白 + 偏向一侧的虹膜 ----------
eye = """<defs>
  <radialGradient id="iris" cx="0.35" cy="0.3" r="0.8">
    <stop offset="0" stop-color="#5AA9E6"/><stop offset="0.6" stop-color="#2563A8"/><stop offset="1" stop-color="#112B4C"/>
  </radialGradient>
</defs>
<path d="M 252 512 C 340 330, 684 330, 772 512 C 684 694, 340 694, 252 512 Z" fill="#FFFFFF"/>
<circle cx="600" cy="512" r="122" fill="url(#iris)"/>
<circle cx="600" cy="512" r="56" fill="#0A1626"/>
<circle cx="562" cy="466" r="26" fill="#FFFFFF" opacity="0.92"/>
<circle cx="646" cy="556" r="12" fill="#FFFFFF" opacity="0.45"/>
<circle cx="782" cy="300" r="30" fill="#FF7A66"/>
<circle cx="782" cy="300" r="52" fill="#FF7A66" opacity="0.45" filter="url(#glow)"/>"""
A = frame("#273A6B", "#0B1428", eye,
          """<radialGradient id="iris" cx="0.35" cy="0.3" r="0.8"><stop offset="0" stop-color="#5AA9E6"/><stop offset="0.6" stop-color="#2563A8"/><stop offset="1" stop-color="#112B4C"/></radialGradient>""")

# ---------- B 斜光·弧：斜掠的一道暖光 ----------
beam = """<defs>
  <radialGradient id="orb" cx="0.38" cy="0.3" r="0.85">
    <stop offset="0" stop-color="#33527A"/><stop offset="1" stop-color="#0B1728"/>
  </radialGradient>
  <linearGradient id="beam" x1="0" y1="1" x2="1" y2="0">
    <stop offset="0" stop-color="#FFC46B" stop-opacity="0"/>
    <stop offset="0.45" stop-color="#FFC46B" stop-opacity="0.95"/>
    <stop offset="1" stop-color="#FF8A5B" stop-opacity="0"/>
  </linearGradient>
  <clipPath id="sqclip"><path d=\"""" + SQ + """\"/></clipPath>
</defs>
<circle cx="512" cy="536" r="248" fill="url(#orb)"/>
<circle cx="512" cy="536" r="248" fill="none" stroke="#FFFFFF" stroke-opacity="0.10" stroke-width="3"/>
<g clip-path="url(#sqclip)">
  <path d="M 170 830 C 380 690, 620 430, 860 250" fill="none" stroke="url(#beam)" stroke-width="74" stroke-linecap="round"/>
  <path d="M 170 830 C 380 690, 620 430, 860 250" fill="none" stroke="#FFF3DC" stroke-opacity="0.35" stroke-width="12" stroke-linecap="round"/>
</g>
<circle cx="736" cy="336" r="24" fill="#FFE1A8"/>"""
B = frame("#182B45", "#08101D", beam, """<radialGradient id="orb" cx="0.38" cy="0.3" r="0.85"><stop offset="0" stop-color="#33527A"/><stop offset="1" stop-color="#0B1728"/></radialGradient>
  <linearGradient id="beam" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="#FFC46B" stop-opacity="0"/><stop offset="0.45" stop-color="#FFC46B" stop-opacity="0.95"/><stop offset="1" stop-color="#FF8A5B" stop-opacity="0"/></linearGradient>
  <clipPath id="sqclip"><path d=\"""" + SQ + """\"/></clipPath>""")

# ---------- C 三束余光：三张卡 / 一个在等你 ----------
three = """<defs>
  <linearGradient id="sweep" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#FFFFFF" stop-opacity="0"/>
    <stop offset="0.5" stop-color="#FFFFFF" stop-opacity="0.55"/>
    <stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
  </linearGradient>
</defs>
<path d="M 200 372 C 360 268, 664 268, 824 372" fill="none" stroke="url(#sweep)" stroke-width="16" stroke-linecap="round"/>
<circle cx="300" cy="600" r="62" fill="#4FD1C5"/>
<circle cx="512" cy="600" r="62" fill="#4FD1C5" opacity="0.55"/>
<circle cx="724" cy="600" r="62" fill="#FF6B5B"/>
<circle cx="724" cy="600" r="92" fill="#FF6B5B" opacity="0.40" filter="url(#glow)"/>
<rect x="238" y="706" width="124" height="12" rx="6" fill="#FFFFFF" opacity="0.20"/>
<rect x="450" y="706" width="124" height="12" rx="6" fill="#FFFFFF" opacity="0.20"/>
<rect x="662" y="706" width="124" height="12" rx="6" fill="#FFFFFF" opacity="0.20"/>"""
C = frame("#163042", "#08151F", three, """<linearGradient id="sweep" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#FFFFFF" stop-opacity="0"/><stop offset="0.5" stop-color="#FFFFFF" stop-opacity="0.55"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>""")

# ---------- D 瞳孔·月牙（浅色版） ----------
moon = """<defs>
  <radialGradient id="deep" cx="0.38" cy="0.3" r="0.85">
    <stop offset="0" stop-color="#3E5C8A"/><stop offset="1" stop-color="#101F38"/>
  </radialGradient>
  <linearGradient id="crescent" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#8FE3D2"/><stop offset="1" stop-color="#3FA9C9"/>
  </linearGradient>
  <mask id="moonmask"><rect x="0" y="0" width="1024" height="1024" fill="#000"/><circle cx="512" cy="512" r="244" fill="#FFF"/><circle cx="612" cy="452" r="220" fill="#000"/></mask>
</defs>
<circle cx="512" cy="512" r="244" fill="url(#deep)"/>
<circle cx="512" cy="512" r="244" fill="url(#crescent)" mask="url(#moonmask)"/>
<circle cx="512" cy="512" r="244" fill="none" stroke="#0B1A30" stroke-opacity="0.15" stroke-width="4"/>
<circle cx="404" cy="420" r="20" fill="#FFFFFF" opacity="0.7"/>"""
D = frame("#F8F6F1", "#E4E1DA", moon, """<radialGradient id="deep" cx="0.38" cy="0.3" r="0.85"><stop offset="0" stop-color="#3E5C8A"/><stop offset="1" stop-color="#101F38"/></radialGradient>
  <linearGradient id="crescent" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8FE3D2"/><stop offset="1" stop-color="#3FA9C9"/></linearGradient>
  <mask id="moonmask"><rect x="0" y="0" width="1024" height="1024" fill="#000"/><circle cx="512" cy="512" r="244" fill="#FFF"/><circle cx="612" cy="452" r="220" fill="#000"/></mask>""")

for name, svg in (("A_余光之眼", A), ("B_斜光弧", B), ("C_三束余光", C), ("D_瞳孔月牙_浅色", D)):
    (OUT / f"{name}.svg").write_text(svg, encoding="utf-8")
    print("wrote", name + ".svg")
