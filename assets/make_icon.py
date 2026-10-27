from PIL import Image, ImageDraw, ImageFilter
import math, os

S=1024
img=Image.new("RGBA",(S,S),(0,0,0,0))
d=ImageDraw.Draw(img)

# rounded dark tile with vertical purple/indigo gradient
tile=Image.new("RGBA",(S,S),(0,0,0,0))
td=ImageDraw.Draw(tile)
for y in range(S):
    t=y/(S-1)
    r=int(18+(36-18)*t); g=int(10+(17-10)*t); b=int(34+(65-34)*t)
    td.line([(0,y),(S,y)],fill=(r,g,b,255))
mask=Image.new("L",(S,S),0)
ImageDraw.Draw(mask).rounded_rectangle([10,10,S-10,S-10],radius=190,fill=255)
img.paste(tile,(0,0),mask)
d=ImageDraw.Draw(img)

# subtle glow rings
for radius,alpha,width in [(340,40,5),(390,24,3),(300,26,2)]:
    d.ellipse([S/2-radius,S/2-radius,S/2+radius,S/2+radius],outline=(147,51,234,alpha),width=width)

# microphone capsule
capsule=Image.new("RGBA",(S,S),(0,0,0,0))
cd=ImageDraw.Draw(capsule)
cd.rounded_rectangle([392,180,632,560],radius=120,fill=(24,18,44,255),outline=(236,72,153,255),width=14)
cd.rounded_rectangle([418,206,606,534],radius=96,fill=(38,27,68,255),outline=(34,211,238,180),width=5)
# mic grille slats
for i in range(7):
    y=270+i*38
    cd.line([(445,y),(579,y)],fill=(244,114,182,190),width=7)
# mic arc and stem
cd.arc([330,270,694,680],200,340,fill=(34,211,238,255),width=16)
cd.line([(512,690),(512,780)],fill=(236,72,153,255),width=16)
cd.rounded_rectangle([430,770,594,800],radius=15,fill=(34,211,238,255))
img.alpha_composite(capsule)

# neon waveform crossing the mic
pts=[]
for i in range(0,101):
    t=i/100
    x=120+784*t
    # gaussian envelope with several peaks
    env=math.exp(-((t-0.5)/0.32)**2)
    y=470 + env*(math.sin(t*math.pi*8.0)*95 + math.sin(t*math.pi*21.0)*35)
    pts.append((x,y))
d.line(pts,fill=(34,211,238,255),width=14,joint="curve")
# magenta routing cable from left node to right node
cable=[]
for i in range(0,101):
    t=i/100
    x=90+844*t
    y=830 - 120*math.sin(math.pi*t)
    cable.append((x,y))
d.line(cable,fill=(236,72,153,255),width=11,joint="curve")
for cx,cy in [(96,830),(928,830)]:
    d.ellipse([cx-22,cy-22,cx+22,cy+22],fill=(18,10,34,255),outline=(34,211,238,255),width=9)
    d.ellipse([cx-8,cy-8,cx+8,cy+8],fill=(236,72,153,255))

# soft outer glow on neon strokes
glow=img.filter(ImageFilter.GaussianBlur(26))
glow.putalpha(glow.getchannel("A").point(lambda v: int(v*0.55)))
out=Image.alpha_composite(glow,img)
out.save(r"K:\AI\NL Audio Matrix\assets\nlmatrix_icon_big.png")

for size,name in [(48,"nlmatrix_icon_small.png"),(256,"nlmatrix_icon_256.png")]:
    out.resize((size,size),Image.LANCZOS).save(rf"K:\AI\NL Audio Matrix\assets\{name}")
out.resize((256,256),Image.LANCZOS).save(r"K:\AI\NL Audio Matrix\assets\nlmatrix_icon.ico",sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print("icons generated")
