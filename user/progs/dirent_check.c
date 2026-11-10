/* Guest-compiled acceptance helper; raw syscalls independently check the ABI,
 * and the released runtime's directory iterator must return the same names. */
package main;
import runtime;
static int lengths[14] = {1,27,28,29,30,30,31,31,32,63,127,128,254,255};
static int maximum;
static int count;
static int failed;
static void expect(int ok, const char *why) {
    if (!ok) { runtime.printf("DENTFAIL %s\n", why); failed = 1; }
}
static int equal(const char *a, const char *b) {
    int i=0;while(a[i] && a[i]==b[i])i++;return a[i]==b[i];
}
static void filename(int index, char *out) {
    int i;int n;
    if(index==count) { for(i=0;i<maximum;i++)out[i]='d';out[i]=0;return; }
    n=lengths[index];for(i=0;i<n;i++)out[i]='n';
    if(index==4)out[n-1]='a';if(index==5)out[n-1]='b';
    if(index==6)out[n-1]='a';
    out[n]=0;
}
static void path_join(char *out,const char *base,const char *name) {
    int i=0,j=0;while(base[i]) {out[i]=base[i];i++;}out[i++]='/';
    while(name[j])out[i++]=name[j++];out[i]=0;
}
static void file_bytes(const char *path,int index,int seed) {
    char data[100];char got[101];int i;int len=40+index;long fd;long n;
    for(i=0;i<len;i++)data[i]=(char)('A'+(i+index)%26);
    if(seed) {
        fd=__syscall(2,(long)path,193,0644);
        expect(fd>=0,"create full distinct name");if(fd<0)return;
        expect(__syscall(1,fd,(long)data,len)==len,"write bytes");
        expect(__syscall(3,fd)==0,"close write");
    }
    fd=__syscall(2,(long)path,0,0);
    expect(fd>=0,"open full distinct name");if(fd<0){runtime.printf("DENTFAIL file index=%d\n",index);return;}
    expect(__syscall(217,fd,(long)got,48)<0,"regular file is not a directory");
    n=__syscall(0,fd,(long)got,101);expect(n==len,"size and file offset retained");
    if(n==len)for(i=0;i<len;i++)expect(got[i]==data[i],"exact file bytes");
    expect(__syscall(3,fd)==0,"close read");
}
static int name_index(const char *name) {
    int i;char expected[256];
    for(i=0;i<=count;i++){filename(i,expected);if(equal(name,expected))return i;}
    return -1;
}
static unsigned long number(const char *p) {
    unsigned long n=0;int i;for(i=0;i<8;i++)n|=(unsigned long)(unsigned char)p[i]<<(i*8);return n;
}
static void raw_scan(const char *base,int cap,int fault) {
    char arena[2064];char *buf=arena+8;long fd;long n;long off;
    unsigned long mask=0;unsigned long cookie=0;int calls=0;int i;
    fd=__syscall(2,(long)base,0x10000,0);expect(fd>=0,"open directory");if(fd<0)return;
    if(fault)expect(__syscall(217,fd,-16,48)<0,"fault/wrap does not consume cookie");
    if(cap<48) {
        expect(__syscall(217,fd,(long)buf,cap)<0,"undersized first buffer is error, not EOF");
        cap=280;
    }
    while(calls++<100) {
        for(i=0;i<2064;i++)arena[i]=(char)0xa5;
        n=__syscall(217,fd,(long)buf,cap);
        if(n<0) {
            /* A pending long record cannot fit; the next larger request must
             * recover it, not skip it or falsely report EOF. */
            expect(cap<280,"unexpected directory read error");
            n=__syscall(217,fd,(long)buf,280);
            expect(n>0,"pending record survives too-small buffer");
        }
        if(n<=0)break;
        expect(n<=2048,"bounded output");if(n>2048)break;
        for(i=0;i<8;i++)expect((unsigned char)arena[i]==0xa5,"leading buffer guard");
        for(i=(int)n+8;i<2064;i++)expect((unsigned char)arena[i]==0xa5,"trailing buffer guard");
        off=0;
        while(off<n) {
            unsigned long rec;int len=0;int index;int wanted;
            expect(n-off>=24,"whole record header");if(n-off<24)break;
            rec=(unsigned char)buf[off+16]|((unsigned long)(unsigned char)buf[off+17]<<8);
            expect(rec>=24 && !(rec&7) && rec<=(unsigned long)(n-off),"exact record boundary");
            if(rec<24 || (rec&7) || rec>(unsigned long)(n-off))break;
            while(19+len<(int)rec && buf[off+19+len])len++;
            expect(len>0 && len<=255 && 19+len<(int)rec,"complete terminated name");
            if(!len || len>255 || 19+len>=(int)rec)break;
            wanted=(19+len+1+7)&~7;if(wanted<48)wanted=48;
            expect(rec==(unsigned long)wanted,"record size matches full name");
            for(i=19+len;i<(int)rec;i++)expect(!buf[off+i],"zero padding");
            expect(number(buf+off)>0,"real inode");
            cookie++;expect(number(buf+off+8)==cookie,"entry cookie, not byte count");
            if(!equal(buf+off+19,".") && !equal(buf+off+19,"..")) {
                index=name_index(buf+off+19);
                expect(index>=0,"no clipped or invented names");
                if(index>=0) {
                    expect(!(mask&(1UL<<index)),"no duplicate entries");mask|=1UL<<index;
                    expect((unsigned char)buf[off+18]==(index==count?4:8),"actual entry type");
                }
            }
            off+=rec;
        }
        expect(off==n,"no partial final record");
    }
    expect(calls<100,"enumeration terminates");
    expect(mask==((1UL<<(count+1))-1),"all colliding names returned once");
    expect(__syscall(217,fd,(long)buf,280)==0,"stable EOF");
    expect(__syscall(3,fd)==0,"directory close");
}
static void runtime_scan(const char *base) {
    void *dir=runtime.__fakecc_dir_open(base);const char *name;
    unsigned long mask=0;int calls=0;int index;
    expect(dir!=0,"runtime directory open");if(!dir)return;
    while((name=runtime.__fakecc_dir_next(dir))!=0 && calls++<100) {
        if(equal(name,".") || equal(name,".."))continue;
        index=name_index(name);expect(index>=0,"runtime lossless names");
        if(index>=0){expect(!(mask&(1UL<<index)),"runtime no duplicate");mask|=1UL<<index;}
    }
    expect(mask==((1UL<<(count+1))-1),"runtime all names");
    expect(runtime.__fakecc_dir_close(dir)==0,"runtime close");
}
/* Short argv roots exercise paths that cannot themselves fit exec's127-byte
 * argument slots. The private Ext2 fixture has enough inodes for250 levels. */
static int path_fixture(const char *mode,const char *base,int deep) {
    char path[514];char name[256];int n=0;int i;int seed;
    seed=equal(mode,deep?"deep-seed":"path-seed");
    while(base[n]){path[n]=base[n];n++;}path[n]=0;
    expect(n==9,"bounded fixture root length");if(n!=9)return 2;
    if(seed)expect(__syscall(83,(long)path,0755)==0,"fixture root mkdir");
    if(deep) {
        for(i=0;i<250;i++) {
            path[n++]='/';path[n++]='a';path[n]=0;
            if(seed)expect(__syscall(83,(long)path,0755)==0,"deep mkdir");
        }
        path[n++]='/';path[n++]='f';path[n]=0;
    } else {
        for(i=0;i<255;i++)name[i]='p';name[i]=0;
        path_join(path,base,name);n=0;while(path[n])n++;
        if(seed)expect(__syscall(83,(long)path,0755)==0,"long-path dirname");
        path[n++]='/';for(i=0;i<245;i++)path[n++]='q';path[n]=0;
    }
    expect(n==511,"exact maximum path length");
    file_bytes(path,8,seed);
    path[n++]='x';path[n]=0;
    expect(__syscall(2,(long)path,0,0)<0,"512-byte path read rejected");
    expect(__syscall(2,(long)path,577,0644)<0,"512-byte path create/truncate rejected");
    path[--n]=0;file_bytes(path,8,0);
    if(!failed)runtime.printf("PATHCHECK PASS %s deep=%d length=%d\n",base,deep,n);
    return failed?1:0;
}
int main(int argc,char **argv) {
    char name[257];char path[512];char folder[512];char nested[512];
    int i;int seed;int limit;int caps[10]={1,23,47,48,55,56,279,280,281,1920};
    if(argc!=4 && argc!=5)return 2;
    if(equal(argv[1],"deep-seed") || equal(argv[1],"deep-check"))
        return path_fixture(argv[1],argv[2],1);
    if(equal(argv[1],"path-seed") || equal(argv[1],"path-check"))
        return path_fixture(argv[1],argv[2],0);
    maximum=equal(argv[3],"31")?31:255;count=maximum==31?8:14;
    /* Fixture name range and destination FS capacity differ after cross-FS
     * copying Ramfs names to Ext2. Never ask Ext2 to reject a valid32-byte name. */
    limit=argc==5?(equal(argv[4],"31")?31:255):maximum;
    seed=equal(argv[1],"seed");
    for(i=0;i<count;i++){filename(i,name);path_join(path,argv[2],name);file_bytes(path,i,seed);}
    filename(count,name);path_join(folder,argv[2],name);
    if(seed)expect(__syscall(83,(long)folder,0755)==0,"max name directory");
    for(i=3;i<8;i++){filename(i,name);path_join(nested,folder,name);file_bytes(nested,i+32,seed);}
    /* Adding a directory after a full first block must not erase existing
     * later-block files. Check BEFORE negative opens to isolate this case. */
    for(i=0;i<count;i++){filename(i,name);path_join(path,argv[2],name);file_bytes(path,i,0);}
    for(i=0;i<limit+1;i++)name[i]='n';name[i]=0;path_join(path,argv[2],name);
    expect(__syscall(2,(long)path,0,0)<0,"over-limit name must not alias existing max prefix");
    expect(__syscall(2,(long)path,577,0644)<0,"over-limit create/truncate rejected");
    filename(count-1,name);path_join(path,argv[2],name);file_bytes(path,count-1,0);
    for(i=0;i<10;i++)raw_scan(argv[2],caps[i],i==0);
    runtime_scan(argv[2]);
    if(!failed)runtime.printf("DENTCHECK PASS %s max=%d\n",argv[2],maximum);
    return failed?1:0;
}
