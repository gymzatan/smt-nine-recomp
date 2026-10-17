"""Replace encoded video using original XMVFile packet identities and original audio."""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

BASE=Path(__file__).resolve().parent
ROOT=BASE.parents[1]
sys.path.insert(0,str(ROOT/'vendor/xboxrecomp/tools/xmv'))
from xmv_demux import XMVFile


def probe_bytes(value):
    return bytes.fromhex(''.join(line.split(':',1)[1].split('  ',1)[0].strip()
        for line in value.strip().splitlines()))


def audio_data(movie,packet,track):
    start=packet.audio_offsets[track]
    return movie.data[start:start+packet.audio_data_sizes[track]]


def write(source,encoded,output):
    movie=XMVFile(source)
    probe=subprocess.run(['ffprobe','-v','error','-select_streams','v:0',
        '-show_streams','-show_packets','-show_data','-of','json',str(encoded)],capture_output=True,check=True)
    data=json.loads(probe.stdout);stream=data['streams'][0]
    assert stream['codec_name']=='wmv2' and (stream['width'],stream['height'])==(movie.width,movie.height)
    frames=[probe_bytes(packet['data']) for packet in data['packets']]
    assert len(frames)==movie.total_frames
    assert all(len(frame)==int(packet['size']) for frame,packet in zip(frames,data['packets']))
    extra=probe_bytes(stream['extradata']);assert len(extra)==4
    flags=int.from_bytes(extra,'big')
    # The inverse of the existing XMVFile / FFmpeg documented flag projection.
    xmv_flags=sum(((flags>>bit)&1)<<index for index,bit in enumerate((15,14,13,12,11,10)))|(((flags>>7)&7)<<6)
    chunks=[];cursor=0
    for pi,packet in enumerate(movie.packets):
        original_header=movie.data[packet.offset:packet.offset+12+movie.audio_track_count*4]
        video=bytearray(struct.pack('<I',xmv_flags) if packet.has_extradata else b'')
        for original in packet.frames:
            frame=frames[cursor];cursor+=1
            padded=frame+b'\0'*((-len(frame))%4)
            assert 4<=len(padded)<=0x80000
            original_fheader=struct.unpack_from('<I',movie.data,original.offset-4)[0]
            fheader=(original_fheader&0xfffe0000)|((len(padded)-4)//4)
            video.extend(struct.pack('<I',fheader))
            video.extend(b''.join(padded[i:i+4][::-1] for i in range(0,len(padded),4)))
        audios=[audio_data(movie,packet,t) for t in range(movie.audio_track_count)]
        first_header=movie.header_size if pi==0 else 0
        required=len(original_header)+len(video)+sum(map(len,audios))
        # Retain packet allocation where it fits; enlarge the existing packet
        # on the same sector boundary when the new encoded frames require it.
        size=max(packet.size,((required+first_header+4095)//4096)*4096-first_header)
        video.extend(bytes(size-required))
        word=(struct.unpack_from('<I',original_header,4)[0]&0xff800000)|(len(video)+4*movie.audio_track_count)
        assert len(video)+4*movie.audio_track_count<0x800000
        header=bytearray(original_header);struct.pack_into('<I',header,4,word)
        chunks.append(header+video+b''.join(audios));assert len(chunks[-1])==size
    assert cursor==len(frames)
    for i,chunk in enumerate(chunks): struct.pack_into('<I',chunk,0,len(chunks[i+1]) if i+1<len(chunks) else 0)
    header=bytearray(movie.data[:movie.header_size])
    struct.pack_into('<III',header,0,len(chunks[1]) if len(chunks)>1 else 0,
        movie.header_size+len(chunks[0]),max(movie.max_packet_size,max(len(c)+(movie.header_size if i==0 else 0) for i,c in enumerate(chunks))))
    assert movie.packets[-1].offset+movie.packets[-1].size==len(movie.data),'Unexpected unparsed tail'
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(header+b''.join(chunks))
    rebuilt=XMVFile(output)
    assert rebuilt.total_frames==movie.total_frames and len(rebuilt.packets)==len(movie.packets)
    assert (rebuilt.width,rebuilt.height,rebuilt.duration_ms,rebuilt.version)==(movie.width,movie.height,movie.duration_ms,movie.version)
    assert rebuilt.data[12:movie.header_size]==movie.data[12:movie.header_size]
    assert rebuilt.audio_track_count==movie.audio_track_count
    audio_hashes=[]
    for t in range(movie.audio_track_count):
        old=hashlib.sha256();new=hashlib.sha256()
        for a,b in zip(movie.packets,rebuilt.packets):
            old_bytes=audio_data(movie,a,t);new_bytes=audio_data(rebuilt,b,t)
            assert old_bytes==new_bytes,(t,a.offset)
            old.update(old_bytes);new.update(new_bytes)
        assert old.digest()==new.digest();audio_hashes.append(old.hexdigest())
    for a,b in zip(movie.packets,rebuilt.packets):
        assert a.frame_count==b.frame_count and len(a.frames)==len(b.frames)
        assert movie.data[a.offset+8:a.offset+12+movie.audio_track_count*4]==rebuilt.data[b.offset+8:b.offset+12+movie.audio_track_count*4]
        for af,bf in zip(a.frames,b.frames):
            old=struct.unpack_from('<I',movie.data,af.offset-4)[0];new=struct.unpack_from('<I',rebuilt.data,bf.offset-4)[0]
            assert old>>17==new>>17
    receipt={'passed':True,'source':str(source),'encoded':str(encoded),'output':str(output),
        'source_sha256':hashlib.sha256(movie.data).hexdigest(),'output_sha256':hashlib.sha256(rebuilt.data).hexdigest(),
        'frames':movie.total_frames,'packets':len(chunks),'width':movie.width,'height':movie.height,'duration_ms':movie.duration_ms,
        'audio_track_count':movie.audio_track_count,'audio_stream_sha256':audio_hashes,'native_frame_timestamp_deltas_preserved':True,
        'packet_identities_and_audio_partition_preserved':True,'all_audio_payload_bytes_preserved':True,
        'wmv2_extradata':extra.hex(),'xmv_codec_flags':xmv_flags,'max_packet_size':rebuilt.max_packet_size,
        'native_parser':'vendor/xboxrecomp/tools/xmv/xmv_demux.py','runtime_verified':False,'deployed':False}
    output.with_suffix('.receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(receipt,ensure_ascii=False),flush=True)
    return receipt


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--encoded',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();write(args.source,args.encoded,args.output)
