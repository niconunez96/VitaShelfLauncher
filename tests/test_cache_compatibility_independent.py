"""Cross-version cache fixtures, including rejection byte invariants."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
C = r"""
#include "catalog_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
 char id[VP_TEXT],title[VP_TEXT],platform[VP_TEXT],platform_display[VP_TEXT],description[VP_TEXT];
 char launch_target[VP_PATH],remote_id[32],fs_name[VP_TEXT],download_url[VP_URL],cover_url[VP_URL];
 VpSource source; VpKind kind; bool installed,favorite,multi_file; unsigned long last_played;
} V1;
typedef struct { V1 prefix; char background_url[VP_URL]; } V2;
static VpCatalog actual;
int main(int argc,char **argv) {
 assert(argc==4); int version=atoi(argv[2]);
 if (!strcmp(argv[3],"write")) {
  FILE *f=fopen(argv[1],"wb"); assert(f);
  for(size_t i=0;i<VP_MAX_ITEMS;i++) {
   V2 old={0}; snprintf(old.prefix.id,sizeof(old.prefix.id),"romm:%zu",i);
   old.prefix.favorite=(i%2)!=0; old.prefix.installed=true; old.prefix.last_played=100+i;
   strcpy(old.background_url,"https://example.test/hero.png");
   if(version<3) assert(fwrite(&old,version==1?sizeof(V1):sizeof(V2),1,f)==1);
   else { memcpy(&actual.items[i],&old,sizeof(old)); actual.items[i].hidden=true; }
  }
  size_t count=VP_MAX_ITEMS;
  if(version==3) { actual.count=count; assert(fwrite(&actual,sizeof(actual),1,f)==1); }
  else assert(fwrite(&count,sizeof(count),1,f)==1);
  assert(!fclose(f)); return 0;
 }
 FILE *f=fopen(argv[1],"rb"); assert(f);
 bool valid=vp_catalog_cache_read(f,(unsigned)version,&actual); assert(!fclose(f));
 if(!strcmp(argv[3],"valid")) {
  assert(valid && actual.count==VP_MAX_ITEMS);
  for(size_t i=0;i<actual.count;i++) { char id[VP_TEXT]; snprintf(id,sizeof(id),"romm:%zu",i);
   assert(!strcmp(id,actual.items[i].id) && actual.items[i].favorite==((i%2)!=0));
   assert(actual.items[i].installed && actual.items[i].last_played==100+i);
   assert(actual.items[i].hidden==(version==3));
   assert(!strcmp(actual.items[i].background_url,version==1?"":"https://example.test/hero.png"));
  }
 } else assert(!valid);
 puts(valid?"accepted":"rejected"); return 0;
}
"""


class CacheCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="vitashelf-cache-compatibility-")
        cls.directory = Path(cls.temporary.name)
        cls.binary = cls.directory / "cache-compatibility"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-Iinclude", "-Isrc/vita", "-x", "c", "-", "src/vita/catalog_cache.c",
                        "-o", str(cls.binary)], cwd=ROOT, input=C, text=True, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_reader(self, path, version, valid):
        before = hashlib.sha256(path.read_bytes()).digest()
        result = subprocess.run([str(self.binary), str(path), str(version), "valid" if valid else "invalid"],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "accepted\n" if valid else "rejected\n")
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)

    def test_legacy_and_current_full_catalogues(self):
        for version in (1, 2, 3):
            with self.subTest(version=version):
                path = self.directory / f"v{version}.cache"
                subprocess.run([str(self.binary), str(path), str(version), "write"], check=True)
                self.run_reader(path, version, True)
                original = path.read_bytes()
                path.write_bytes(original[:100])
                self.run_reader(path, version, False)
                path.write_bytes(original)
                self.run_reader(path, 99, False)


if __name__ == "__main__":
    unittest.main()
