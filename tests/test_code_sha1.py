"""Image/payload hashing agrees with an independent SHA1 across padding boundaries."""
from pathlib import Path
import hashlib
import subprocess
import pytest


@pytest.fixture(scope='module')
def sha1_binary(tmp_path_factory):
    root=Path(__file__).resolve().parents[1];out=tmp_path_factory.mktemp('sha1')
    source=out/'sha1.c'
    source.write_text('''#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
    FILE *f;long size;unsigned prefix,i;uint8_t *data,digest[20];
    if(argc!=3)return 2;
    prefix=(unsigned)strtoul(argv[2],NULL,10);
    f=fopen(argv[1],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)||(size=ftell(f))<0||fseek(f,0,SEEK_SET))return 2;
    data=malloc((size_t)size+prefix+1);if(!data)return 2;
    if(fread(data+prefix,1,(size_t)size,f)!=(size_t)size)return 2;
    fclose(f);
    code_sha1(size==0&&prefix==0?NULL:data+prefix,(size_t)size,digest);
    for(i=0;i<20;++i)printf("%02x",digest[i]);
    putchar('\\n');free(data);return 0;
}
''')
    binary=out/'sha1'
    subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror',
                    '-ffunction-sections','-fdata-sections','-I'+str(root/'runtime'),str(source),
                    str(root/'runtime/cpu.c'),'-Wl,--gc-sections','-o',str(binary)],check=True)
    return binary


@pytest.mark.parametrize('size',[0,1,3,55,56,57,63,64,65,119,120,127,128,129,511,65536,501056,1000000])
@pytest.mark.parametrize('prefix',[0,1,3])
def test_image_sha1_padding_and_unaligned_input(sha1_binary,tmp_path,size,prefix):
    data=(bytes(range(256))*(size//256+1))[:size];image=tmp_path/'data';image.write_bytes(data)
    result=subprocess.check_output([str(sha1_binary),str(image),str(prefix)],text=True).strip()
    assert result==hashlib.sha1(data).hexdigest()
