"""Execute the driver's vdrm_postclose body with mocked kernel types.
Not a kernel test: no real drm_file lifecycle, no real concurrency.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Postclose(unittest.TestCase):
    def test_extracted_postclose(self):
        src = (ROOT / 'driver/vdrm_ko.c').read_text()
        decl = 'static void vdrm_postclose(struct drm_device *dev, struct drm_file *file)'
        body = src.split(decl, 1)[1]
        body = body[: body.index('\n}\n') + 2]   # body keeps its own braces
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define VDRM_POOL_MAX 3
#define VDRM_ST_FREE 0
#define VDRM_ST_TAKEN 1
#define VDRM_ST_DISPL 2
#define VDRM_ST_TAKEN_ 1
#define pr_info(...) ((void)0)
struct drm_device { int _x; };
struct drm_file { int _x; };
struct vdrm_pool_entry { u32 used; u32 state; u32 id; u32 allocation_reserved; struct drm_file *allocation_owner; };
static struct vdrm_pool_entry g_pool[VDRM_POOL_MAX];
static int g_pool_lock, locked;
static u32 g_postclose_releases;
static void mutex_lock(int *m){(void)m;assert(!locked);locked=1;}
static void mutex_unlock(int *m){(void)m;assert(locked);locked=0;}
''' + decl + body + r'''

int main(void) {
    struct drm_file A, B; void *pa=&A, *pb=&B;
    /* slot0: TAKEN by A -> should become FREE, unreserved
       slot1: DISPL by A -> reservation cleared, display state kept
       slot2: TAKEN by B -> untouched
       slot3: unused -> untouched */
    g_pool[0]=(struct vdrm_pool_entry){1,VDRM_ST_TAKEN,10,1,pa};
    g_pool[1]=(struct vdrm_pool_entry){1,VDRM_ST_DISPL,11,1,pa};
    g_pool[2]=(struct vdrm_pool_entry){1,VDRM_ST_TAKEN,12,1,pb};
    vdrm_postclose(0, pa);
    assert(!locked);
    assert(g_pool[0].state==VDRM_ST_FREE && !g_pool[0].allocation_reserved && !g_pool[0].allocation_owner);
    assert(g_pool[1].state==VDRM_ST_DISPL && !g_pool[1].allocation_reserved && !g_pool[1].allocation_owner);
    assert(g_pool[2].state==VDRM_ST_TAKEN && g_pool[2].allocation_reserved && g_pool[2].allocation_owner==pb);
    assert(g_postclose_releases==2);
    /* second close of same file: nothing left to release */
    vdrm_postclose(0, pa);
    assert(g_postclose_releases==2);
    /* closing B releases only B's slot */
    vdrm_postclose(0, pb);
    assert(g_pool[2].state==VDRM_ST_FREE && !g_pool[2].allocation_reserved && g_postclose_releases==3);
    /* unused entry never counted */
    vdrm_postclose(0, pa);
    assert(g_postclose_releases==3);
    puts("postclose assertions passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'pc.c'; exe = Path(tmp) / 'pc'
            c.write_text(code)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    unittest.main()
