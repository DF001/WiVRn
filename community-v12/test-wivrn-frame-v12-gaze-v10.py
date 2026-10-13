#!/usr/bin/env python3
"""Static patch guards + compile/run the exact V10 source-selection block in C++20."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('patchv10', root/'create-wivrn-frame-v12-gaze-v10.py')
m=importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

fake='''Steam Frame gaze v9 active: timestamp-corrected binocular world-space mmap + adaptive micro/medium/large sample fixation (1.15/2.40/5.00 deg), 144Hz temporal smoothing and 250ms stale-source timeout
class steam_frame_eye_mmap {
	enum class source
	{
		combined,
		held,
	};
void log_source(source current_source, float left_unc, float right_unc) {
switch(current_source) {
			case source::combined:
				break;
			case source::held:
				spdlog::warn("Steam Frame gaze v9 source: HOLD last stable combined gaze");
				break;
}
}
if (combined)
'''
# Build a representative v9 reader policy as an anchor fixture.
fixture = '''
			if (combined)
			{
				if (not last_good_direction)
				{
					source_ready = true;
					recovery_samples = recovery_samples_required;
				}
				else if (not source_ready)
				{
					++recovery_samples;
					if (recovery_samples >= recovery_samples_required)
					{
						source_ready = true;
					}
				}
				if (source_ready)
				{
					last_good_direction = *combined;
					last_good_sample_time_raw = timestamp;
					last_good_sequence = sequence;
					current_source = source::combined;
					log_source(current_source, left_unc, right_unc);
				}
				else
				{
					current_source = source::held;
					log_source(current_source, left_unc, right_unc);
				}
			}
			else
			{
				source_ready = false;
				recovery_samples = 0;
				current_source = source::held;
				log_source(current_source, left_unc, right_unc);
			}
		}

		if (last_good_direction)'''
source='''Steam Frame gaze v9 active: timestamp-corrected binocular world-space mmap + adaptive micro/medium/large sample fixation (1.15/2.40/5.00 deg), 144Hz temporal smoothing and 250ms stale-source timeout
	enum class source
	{
		combined,
		held,
	};
'''
source += '''\t\t\tcase source::held:\n\t\t\t\tspdlog::warn(\n'''+fixture
source+='\nWIVRN_FRAME_GAZE_V9_MICRO_SAMPLES\nreturn uint32_t(5);\nframe_world_stale_timeout_ns\nglm::conjugate(*current_head) * stabilized_world\neye-server.mmap\nno fresh binocular gaze for >250ms\n'
changed=m.patch(source)
assert 'Steam Frame gaze v10 active' in changed
assert 'source::left' in changed and 'source::right' in changed
assert 'WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES' in changed
assert 'return uint32_t(3);' in changed
assert 'Steam Frame gaze v9' not in changed

cpp='''#include <cassert>
#include <cstdint>
#include <optional>
namespace glm { struct vec3 {float x=0,y=0,z=0;}; }
enum class source {combined,left,right,held};
struct Reader {
 static constexpr uint32_t recovery_samples_required=3;
 std::optional<glm::vec3> last_good_direction;
 double last_good_sample_time_raw=0;
 uint32_t last_good_sequence=0;
 bool source_ready=false;
 uint32_t recovery_samples=0;
 source current_source=source::held;
 void log_source(source,float,float) {}
 void process(std::optional<glm::vec3> left, std::optional<glm::vec3> right, bool left_good, bool right_good, std::optional<glm::vec3> combined, double timestamp, uint32_t sequence) {
 const float left_unc=0.001f, right_unc=0.001f;
'''+m.SINGLE_EYE_POLICY+'''
 }
};
int main() {
 glm::vec3 L{1,0,-1}, R{0,1,-1}, B{0,0,-1};
 Reader a;
 a.process(L,R,true,true,B,1.0,1);
 assert(a.current_source==source::combined && a.last_good_sequence==1);
 a.process(L,R,true,false,std::nullopt,2.0,2);
 assert(a.current_source==source::left && a.last_good_sequence==2);
 a.process(L,R,false,true,std::nullopt,3.0,3);
 assert(a.current_source==source::right && a.last_good_sequence==3);
 a.process(L,R,false,false,std::nullopt,4.0,4);
 assert(a.current_source==source::held && a.last_good_sequence==3);
 a.process(L,R,false,true,std::nullopt,5.0,5);
 assert(a.current_source==source::held && a.last_good_sequence==3);
 a.process(L,R,false,true,std::nullopt,6.0,6);
 assert(a.current_source==source::held && a.last_good_sequence==3);
 a.process(L,R,false,true,std::nullopt,7.0,7);
 assert(a.current_source==source::right && a.last_good_sequence==7);
 Reader b;
 b.process(L,R,false,true,std::nullopt,8.0,8);
 assert(b.current_source==source::right && b.last_good_sequence==8);
}
'''
with tempfile.TemporaryDirectory() as td:
 cppfile=Path(td)/'mono.cpp'
 exe=Path(td)/'mono'
 cppfile.write_text(cpp)
 subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-O2',str(cppfile),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: V10 patch anchor fixture, C++20 compile, combined/left/right/hold/reacquisition cases')
