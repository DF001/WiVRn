#!/usr/bin/env python3
"""Offline tests: exact V12->V13 source conversion and compiled C++20 selection logic.
NOT a complete WiVRn cross-compile, real producer test, or hardware validation.
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

module_file = Path(__file__).with_name('apply-v13-gaze-v11.py')
spec = importlib.util.spec_from_file_location('v13', module_file)
v13 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v13)

fixture = '''Steam Frame gaze v10 active: timestamp-corrected binocular+monocular world-space mmap with quality hysteresis, adaptive 3-sample micro fixation (1.15/2.40/5.00 deg), 144Hz smoothing and 250ms stale timeout
class steam_frame_eye_mmap {
 enum class source { combined, left, right, held };
\tstatic constexpr uint32_t recovery_samples_required = 3;
\tsource current_source = source::held;
void read() {
'''+v13.POLICY_V12+''' }
};
WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES
return uint32_t(3);
\t\t}();
\t\treturn samples;
Steam Frame gaze v10 mmap active: ABI v5 COMBINED POST, hold on single-eye loss, 3-sample recovery
frame_world_stale_timeout_ns = 250
glm::conjugate(*current_head) * stabilized_world
/dev/shm/eye-server.mmap
'''
out = v13.patch(fixture)
assert 'Steam Frame gaze v11 active:' in out
assert 'WIVRN_FRAME_V13_MONOCULAR' in out
assert 'WIVRN_FRAME_GAZE_V11_MICRO_SAMPLES' in out
assert 'return uint32_t(5);' in out
assert 'Steam Frame gaze v10' not in out
try:
    v13.patch(out)
    raise AssertionError('double-patching should have been refused')
except ValueError:
    pass

cpp = r'''#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
namespace glm {
struct vec3 {float x, y, z;};
float dot(const vec3&a,const vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
}
namespace spdlog { template <class...T> void warn(const char*,T...){ } template <class...T> void info(const char*,T...){ } }
enum class source {combined,left,right,held};
struct Reader {
 static constexpr uint32_t recovery_samples_required=3;
 static constexpr uint32_t v13_mono_transition_samples_required=3;
 static constexpr float v13_mono_transition_max_degrees=2.0f;
 static bool v13_monocular_enabled() { return mono; }
 static bool mono;
 std::optional<glm::vec3> last_good_direction;
 double last_good_sample_time_raw=0;
 uint32_t last_good_sequence=0;
 bool source_ready=false;
 uint32_t recovery_samples=0;
 source current_source=source::held;
 source last_accepted_source=source::combined;
 std::optional<source> pending_transition;
 uint32_t pending_transition_samples=0;
 uint64_t rejected_mono_jumps=0;
 void log_source(source,float,float) {}
 void process(std::optional<glm::vec3> left,std::optional<glm::vec3> right,
             bool left_good,bool right_good,std::optional<glm::vec3> combined, uint32_t sequence) {
 const float left_unc=0.001f,right_unc=0.001f;
 const double timestamp=double(sequence);
'''+v13.POLICY_V13+r'''
 }
};
bool Reader::mono=false;
int main() {
 glm::vec3 B{0.0f,0.0f,-1.0f}, L{0.01f,0.0f,-0.99995f};
 Reader r;
 r.process(B,B,true,true,B,1);
 assert(r.current_source==source::combined && r.last_good_sequence==1);
 // Default must behave like V11: monocular ignored, last combined held.
 r.process(L,std::nullopt,true,false,std::nullopt,2);
 assert(r.current_source==source::held && r.last_good_sequence==1);
 r.process(B,B,true,true,B,3);
 assert(r.current_source==source::held);
 r.process(B,B,true,true,B,4);
 assert(r.current_source==source::held);
 r.process(B,B,true,true,B,5);
 assert(r.current_source==source::combined && r.last_good_sequence==5);
 Reader::mono=true;
 Reader m;
 m.process(B,B,true,true,B,1);
 m.process(L,std::nullopt,true,false,std::nullopt,2);
 assert(m.current_source==source::held && m.last_good_sequence==1);
 m.process(L,std::nullopt,true,false,std::nullopt,3);
 assert(m.current_source==source::held && m.last_good_sequence==1);
 m.process(L,std::nullopt,true,false,std::nullopt,4);
 assert(m.current_source==source::left && m.last_good_sequence==4);
 // Reject an unsafe large left/right switch, do not mark fresh.
 glm::vec3 far{0.5f,0.0f,-0.8660254f};
 m.process(std::nullopt,far,false,true,std::nullopt,5);
 assert(m.current_source==source::held && m.last_good_sequence==4 && m.rejected_mono_jumps==1);
 m.process(B,B,true,true,B,6);
 assert(m.current_source==source::combined && m.last_good_sequence==6);
}
'''
with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 'policy.cpp'
    binary = Path(td) / 'policy'
    src.write_text(cpp)
    subprocess.run(['g++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: source anchors, repeat-apply protection, C++20 binocular-default, 3-sample mono guard, rejected angular jump, recovery')
