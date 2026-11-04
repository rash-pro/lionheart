"""Count new clear-color holes inside opaque 3D regions of 4x replay images.

Reports the literal black/non-black comparison and an interior count excluding
one native pixel around pre-existing black texture/silhouette boundaries.
"""
import argparse
import json
from pathlib import Path
import struct
import zlib
import numpy as np


def png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('PNG required')
    at, compressed = 8, bytearray()
    while at < len(data):
        size = struct.unpack_from('>I', data, at)[0]
        kind, body = data[at+4:at+8], data[at+8:at+8+size]
        if kind == b'IHDR':
            w,h,depth,color,comp,filt,interlace = struct.unpack('>IIBBBBB', body)
            if (depth,color,comp,filt,interlace) != (8,2,0,0,0):
                raise ValueError('host RGB8 PNG required')
        elif kind == b'IDAT':
            compressed.extend(body)
        at += size+12
    scan = np.frombuffer(zlib.decompress(compressed), dtype=np.uint8).reshape(h,w*3+1)
    if np.any(scan[:,0]):
        raise ValueError('host unfiltered PNG required')
    return scan[:,1:].reshape(h,w,3)


def interior(mask, radius):
    side = radius*2+1
    padded = np.pad(mask.astype(np.uint32), ((radius,radius),(radius,radius)))
    sums = np.pad(padded, ((1,0),(1,0))).cumsum(0).cumsum(1)
    return sums[side:,side:] - sums[:-side,side:] - sums[side:,:-side] + sums[:-side,:-side] == side*side


def region(shape, scene):
    h,w = shape
    scale = w//320
    mask = np.zeros((h,w),dtype=bool)
    rectangles = {'battle':(8,80,312,172), 'world':(8,45,312,205), 'dorm':(65,45,145,205)}
    x0,y0,x1,y1 = rectangles[scene]
    mask[y0*scale:y1*scale,x0*scale:x1*scale] = True
    if scene == 'world':
        mask[132*scale:,220*scale:] = False  # HUD globe, separate from world geometry.
    return mask,scale


def compare(off, on, scene):
    if off.shape != on.shape:
        raise ValueError('scanout dimensions differ')
    roi,scale = region(off.shape[:2],scene)
    opaque = np.max(off,axis=2)>8
    clear = np.max(on,axis=2)<=2
    raw = roi & opaque & clear
    holes = raw & interior(opaque,scale)
    return dict(raw_pixels=int(raw.sum()),interior_pixels=int(holes.sum()),roi_pixels=int(roi.sum()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--off',type=Path,required=True)
    parser.add_argument('--on',type=Path,required=True)
    parser.add_argument('--scene',choices=('battle','world','dorm'),required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--max-crack-pixels',type=int,default=8)
    args = parser.parse_args()
    rows=[]
    for off in sorted(args.off.glob('*.png')):
        frame=int(off.stem.split('-')[-1])
        if not 11<=frame<=20:
            continue
        on=args.on/off.name
        rows.append(dict(frame=frame,**compare(png(off),png(on),args.scene)))
    if len(rows)!=10:
        raise ValueError('ten consecutive completed replay frames required')
    result=dict(scene=args.scene,off=str(args.off),on=str(args.on),frames=rows,
                max_interior=max(r['interior_pixels'] for r in rows),
                total_interior=sum(r['interior_pixels'] for r in rows),
                total_raw=sum(r['raw_pixels'] for r in rows),budget=args.max_crack_pixels)
    result['pass']=result['max_interior']<=args.max_crack_pixels
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('scene','max_interior','total_interior','total_raw','pass')}))
    return 0 if result['pass'] else 1


if __name__=='__main__':
    raise SystemExit(main())
