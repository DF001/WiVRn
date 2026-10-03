/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "application.h"
#include "stream.h"
#include "utils/overloaded.h"
#include "wivrn_packets.h"
#include "xr/body_tracker.h"
#include "xr/face_tracker.h"
#include "xr/fb_body_tracker.h"
#include "xr/to_string.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <magic_enum.hpp>
#include <magic_enum_containers.hpp>
#include <ranges>
#include <spdlog/spdlog.h>
#include <thread>

#if defined(__linux__) && !defined(__ANDROID__)
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include "android/battery.h"
#endif

namespace
{

bool frame_gaze_fix_enabled()
{
	static const bool enabled = [] {
		if (const char * value = std::getenv("WIVRN_FRAME_GAZE_FIX"))
			return std::strcmp(value, "0") != 0;
		return bool(WIVRN_STEAM_FRAME_EXPERIMENTS);
	}();
	return enabled;
}

#if defined(__linux__) && !defined(__ANDROID__)
class steam_frame_eye_mmap
{
public:
	enum class source
	{
		combined,
		held,
	};

	struct sample
	{
		uint32_t sequence = 0;
		glm::vec3 direction{0.0f, 0.0f, -1.0f};
		float left_uncertainty = 0.0f;
		float right_uncertainty = 0.0f;
		source selected = source::combined;
	};

private:
	static constexpr const char * path = "/dev/shm/eye-server.mmap";
	static constexpr size_t header_version = 0x000;
	static constexpr size_t header_initialized = 0x004;
	static constexpr size_t header_mutex = 0x008;
	static constexpr size_t header_sequence = 0x038;

	static constexpr uint32_t supported_version = 5;
	static constexpr size_t expected_size = 0x4f21f;
	static constexpr size_t record_base = 0x157;
	static constexpr size_t producer_state = record_base + 0x00;
	static constexpr size_t sample_time = record_base + 0x05;
	static constexpr size_t post_left = record_base + 0x0d;
	static constexpr size_t post_right = record_base + 0x19;
	static constexpr size_t post_cov = record_base + 0x25;

	static constexpr float eye_lost_threshold = 0.004f;
	static constexpr float eye_found_threshold = 0.0025f;
	static constexpr uint32_t recovery_samples_required = 3;

	int fd = -1;
	uint8_t * data = nullptr;
	size_t size = 0;
	pthread_mutex_t * metadata_mutex = nullptr;

	bool left_good = false;
	bool right_good = false;
	bool source_ready = false;
	bool have_seen_sequence = false;
	uint32_t recovery_samples = 0;
	uint32_t last_seen_sequence = 0;
	std::optional<glm::vec3> last_good_direction;
	uint32_t last_good_sequence = 0;
	source current_source = source::held;
	std::optional<source> last_logged_source;

	template <typename T>
	T load(size_t offset) const
	{
		T value{};
		std::memcpy(&value, data + offset, sizeof(value));
		return value;
	}

	std::array<float, 3> load_vec3(size_t offset) const
	{
		std::array<float, 3> value{};
		std::memcpy(value.data(), data + offset, sizeof(value));
		return value;
	}

	std::array<float, 6> load_cov(size_t offset) const
	{
		std::array<float, 6> value{};
		std::memcpy(value.data(), data + offset, sizeof(value));
		return value;
	}

	static bool finite_vec(const std::array<float, 3> & value)
	{
		for (float component: value)
		{
			if (not std::isfinite(component))
			{
				return false;
			}
		}
		return true;
	}

	static std::optional<glm::vec3> normalize_vec(const std::array<float, 3> & value)
	{
		if (not finite_vec(value))
		{
			return std::nullopt;
		}

		glm::vec3 direction{value[0], value[1], value[2]};
		const float length = glm::length(direction);
		if (not std::isfinite(length) or length < 1e-6f)
		{
			return std::nullopt;
		}
		return direction / length;
	}

	static float uncertainty(const std::array<float, 6> & covariance, bool left)
	{
		const size_t base = left ? 0 : 3;
		const float value = std::max(covariance[base], covariance[base + 2]);
		return std::isfinite(value) ? value : INFINITY;
	}

	static bool update_eye_good(bool current, float value)
	{
		if (not std::isfinite(value))
		{
			return false;
		}
		if (current)
		{
			return value <= eye_lost_threshold;
		}
		return value <= eye_found_threshold;
	}

	bool lock()
	{
		if (metadata_mutex == nullptr)
		{
			return false;
		}

		const int result = pthread_mutex_lock(metadata_mutex);
		if (result == 0)
		{
			return true;
		}
		if (result == EOWNERDEAD)
		{
			const int consistent = pthread_mutex_consistent(metadata_mutex);
			if (consistent == 0)
			{
				return true;
			}
			pthread_mutex_unlock(metadata_mutex);
		}
		return false;
	}

	void unlock()
	{
		pthread_mutex_unlock(metadata_mutex);
	}

	void log_source(source selected, float left_unc, float right_unc)
	{
		if (last_logged_source and *last_logged_source == selected)
		{
			return;
		}
		last_logged_source = selected;

		switch (selected)
		{
			case source::combined:
				spdlog::info(
				        "Steam Frame gaze v5 source: COMBINED POST (left_unc={:.6f}, right_unc={:.6f})",
				        left_unc,
				        right_unc);
				break;
			case source::held:
				spdlog::warn(
				        "Steam Frame gaze v5 source: HOLD last stable combined gaze (left_unc={:.6f}, right_unc={:.6f})",
				        left_unc,
				        right_unc);
				break;
		}
	}

	bool open_mapping()
	{
		fd = ::open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
		{
			spdlog::warn("Steam Frame gaze v5: cannot open {}: {}", path, std::strerror(errno));
			return false;
		}

		struct stat st{};
		if (fstat(fd, &st) != 0)
		{
			spdlog::warn("Steam Frame gaze v5: fstat failed: {}", std::strerror(errno));
			::close(fd);
			fd = -1;
			return false;
		}

		size = size_t(st.st_size);
		if (size < expected_size)
		{
			spdlog::warn(
			        "Steam Frame gaze v5: eye mmap too small ({} bytes, need at least {})",
			        size,
			        expected_size);
			::close(fd);
			fd = -1;
			return false;
		}

		void * mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (mapped == MAP_FAILED)
		{
			spdlog::warn("Steam Frame gaze v5: mmap failed: {}", std::strerror(errno));
			::close(fd);
			fd = -1;
			return false;
		}

		data = static_cast<uint8_t *>(mapped);
		metadata_mutex = reinterpret_cast<pthread_mutex_t *>(data + header_mutex);

		const uint32_t version = load<uint32_t>(header_version);
		const uint32_t initialized = load<uint32_t>(header_initialized);
		if (version != supported_version or initialized != 1)
		{
			spdlog::warn(
			        "Steam Frame gaze v5: unsupported eye mmap (version={}, initialized={})",
			        version,
			        initialized);
			munmap(data, size);
			data = nullptr;
			metadata_mutex = nullptr;
			::close(fd);
			fd = -1;
			return false;
		}

		spdlog::info(
		        "Steam Frame gaze v5 mmap active: ABI v5 COMBINED POST, hold on single-eye loss, 3-sample recovery");
		return true;
	}

public:
	explicit steam_frame_eye_mmap(bool enabled)
	{
		if (enabled)
		{
			open_mapping();
		}
	}

	~steam_frame_eye_mmap()
	{
		if (data != nullptr)
		{
			munmap(data, size);
		}
		if (fd >= 0)
		{
			::close(fd);
		}
	}

	steam_frame_eye_mmap(const steam_frame_eye_mmap &) = delete;
	steam_frame_eye_mmap & operator=(const steam_frame_eye_mmap &) = delete;

	bool active() const
	{
		return data != nullptr;
	}

	std::optional<sample> read()
	{
		if (data == nullptr or not lock())
		{
			return std::nullopt;
		}

		const uint32_t sequence = load<uint32_t>(header_sequence);
		const uint32_t state = load<uint32_t>(producer_state);
		const double timestamp = load<double>(sample_time);
		const auto left_raw = load_vec3(post_left);
		const auto right_raw = load_vec3(post_right);
		const auto covariance = load_cov(post_cov);

		unlock();

		if (state != 1 or not std::isfinite(timestamp))
		{
			return std::nullopt;
		}

		const auto left = normalize_vec(left_raw);
		const auto right = normalize_vec(right_raw);
		const float left_unc = uncertainty(covariance, true);
		const float right_unc = uncertainty(covariance, false);

		const bool new_sequence = not have_seen_sequence or sequence != last_seen_sequence;
		if (new_sequence)
		{
			have_seen_sequence = true;
			last_seen_sequence = sequence;

			left_good = left.has_value() and update_eye_good(left_good, left_unc);
			right_good = right.has_value() and update_eye_good(right_good, right_unc);

			std::optional<glm::vec3> combined;
			if (left_good and right_good and left and right)
			{
				const glm::vec3 sum = *left + *right;
				const float length = glm::length(sum);
				if (std::isfinite(length) and length >= 1e-6f)
				{
					combined = sum / length;
				}
			}

			if (combined)
			{
				if (not last_good_direction)
				{
					// At startup there is nothing useful to hold, so accept the first
					// healthy binocular sample immediately.
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

		if (last_good_direction)
		{
			return sample{
			        .sequence = last_good_sequence,
			        .direction = *last_good_direction,
			        .left_uncertainty = left_unc,
			        .right_uncertainty = right_unc,
			        .selected = current_source,
			};
		}

		return std::nullopt;
	}

	static glm::quat direction_to_quaternion(glm::vec3 direction)
	{
		direction = glm::normalize(direction);
		const glm::vec3 forward{0.0f, 0.0f, -1.0f};
		const float cosine = std::clamp(glm::dot(forward, direction), -1.0f, 1.0f);

		if (cosine < -0.9999f)
		{
			return glm::quat(0.0f, 0.0f, 1.0f, 0.0f);
		}

		const glm::vec3 axis = glm::cross(forward, direction);
		const float s = std::sqrt((1.0f + cosine) * 2.0f);
		const float inverse_s = 1.0f / s;

		return glm::normalize(glm::quat(
		        s * 0.5f,
		        axis.x * inverse_s,
		        axis.y * inverse_s,
		        axis.z * inverse_s));
	}
};
#else
class steam_frame_eye_mmap
{
public:
	struct sample
	{
		uint32_t sequence = 0;
		glm::vec3 direction{0.0f, 0.0f, -1.0f};
	};

	explicit steam_frame_eye_mmap(bool)
	{
	}

	bool active() const
	{
		return false;
	}

	std::optional<sample> read()
	{
		return std::nullopt;
	}

	static glm::quat direction_to_quaternion(glm::vec3)
	{
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}
};
#endif

class frame_gaze_stabilizer
{
	std::optional<glm::quat> fixation;
	std::optional<glm::quat> first_outside;
	XrTime last_update_frame = 0;
	uint64_t fixation_samples = 0;

	uint64_t requests = 0;
	uint64_t frame_updates = 0;
	uint64_t held_requests = 0;
	uint64_t fixation_samples_accepted = 0;
	uint64_t stray_samples_rejected = 0;
	uint64_t fixation_switches = 0;
	uint64_t saccade_bypasses = 0;
	XrTime next_log = 0;

	static constexpr float fixation_radius_rad = 2.75f * M_PI / 180.0f;
	static constexpr float saccade_rad = 5.0f * M_PI / 180.0f;
	static constexpr uint64_t max_fixation_samples = 90;

	static glm::quat align_hemisphere(const glm::quat & reference, glm::quat value)
	{
		if (glm::dot(reference, value) < 0.0f)
		{
			value = glm::quat(-value.w, -value.x, -value.y, -value.z);
		}
		return value;
	}

	static float angular_distance(const glm::quat & a, const glm::quat & b)
	{
		glm::quat aligned = align_hemisphere(a, b);
		float d = std::clamp(glm::dot(a, aligned), 0.0f, 1.0f);
		return 2.0f * std::acos(d);
	}

	static glm::quat normalized_blend(const glm::quat & a, glm::quat b, float alpha)
	{
		b = align_hemisphere(a, b);
		return glm::normalize(glm::quat(
		        a.w + alpha * (b.w - a.w),
		        a.x + alpha * (b.x - a.x),
		        a.y + alpha * (b.y - a.y),
		        a.z + alpha * (b.z - a.z)));
	}

	void start_fixation(glm::quat raw)
	{
		fixation = glm::normalize(raw);
		first_outside.reset();
		fixation_samples = 1;
	}

	void accept_fixation_sample(glm::quat raw)
	{
		float alpha;
		if (fixation_samples < max_fixation_samples)
		{
			alpha = 1.0f / float(fixation_samples + 1);
			++fixation_samples;
		}
		else
		{
			alpha = 1.0f / float(max_fixation_samples);
		}

		fixation = normalized_blend(*fixation, raw, alpha);
		++fixation_samples_accepted;
	}

	void switch_to_outside_fixation(glm::quat raw)
	{
		glm::quat first = *first_outside;
		raw = align_hemisphere(first, raw);
		fixation = glm::normalize(glm::quat(
		        first.w + raw.w,
		        first.x + raw.x,
		        first.y + raw.y,
		        first.z + raw.z));
		first_outside.reset();
		fixation_samples = 2;
		++fixation_switches;
	}

public:
	void apply(
	        from_headset::tracking::pose & pose,
	        XrTime timestamp,
	        XrTime frame_key,
	        bool canonical_sample)
	{
		++requests;

		using flags = from_headset::pose_flags;
		const uint8_t orientation_ok =
		        uint8_t(flags::orientation_valid) | uint8_t(flags::orientation_tracked);
		if ((pose.flags & orientation_ok) != orientation_ok)
		{
			return;
		}

		glm::quat raw(
		        pose.pose.orientation.w,
		        pose.pose.orientation.x,
		        pose.pose.orientation.y,
		        pose.pose.orientation.z);
		raw = glm::normalize(raw);

		if (not fixation)
		{
			start_fixation(raw);
			last_update_frame = frame_key;
			++frame_updates;
		}
		else if (canonical_sample and frame_key != last_update_frame)
		{
			last_update_frame = frame_key;
			++frame_updates;

			const float angle = angular_distance(*fixation, raw);
			if (angle >= saccade_rad)
			{
				start_fixation(raw);
				++saccade_bypasses;
			}
			else if (angle > fixation_radius_rad)
			{
				if (first_outside)
				{
					switch_to_outside_fixation(raw);
				}
				else
				{
					first_outside = raw;
					++stray_samples_rejected;
				}
			}
			else
			{
				first_outside.reset();
				accept_fixation_sample(raw);
			}
		}
		else
		{
			++held_requests;
		}

		pose.pose.orientation = {
		        .x = fixation->x,
		        .y = fixation->y,
		        .z = fixation->z,
		        .w = fixation->w,
		};

		pose.angular_velocity = {};
		pose.flags &= ~uint8_t(flags::angular_velocity_valid);

		if (next_log == 0)
		{
			next_log = timestamp + 5'000'000'000;
		}
		else if (timestamp >= next_log)
		{
			spdlog::info(
			        "Steam Frame gaze v5: requests={}, updates={}, held={}, accepted={}, stray_rejected={}, fixation_switches={}, saccade_bypass={}",
			        requests,
			        frame_updates,
			        held_requests,
			        fixation_samples_accepted,
			        stray_samples_rejected,
			        fixation_switches,
			        saccade_bypasses);
			next_log = timestamp + 5'000'000'000;
		}
	}
};

from_headset::tracking::pose locate_space(device_id device, XrSpace space, XrSpace reference, XrTime time)
{
	XrSpaceVelocity velocity{
	        .type = XR_TYPE_SPACE_VELOCITY,
	};

	XrSpaceLocation location{
	        .type = XR_TYPE_SPACE_LOCATION,
	        .next = &velocity,
	};

	auto res = xrLocateSpace(space, reference, time, &location);

	if (XR_SUCCEEDED(res))
		return {
		        .pose = location.pose,
		        .linear_velocity = velocity.linearVelocity,
		        .angular_velocity = velocity.angularVelocity,
		        .device = device,
		        .flags = from_headset::to_pose_flags(location.locationFlags, velocity.velocityFlags),
		};
	spdlog::warn("xrLocateSpace failed for {}: {}", magic_enum::enum_name(device), xr::to_string(res));
	return {.device = device};
}

class locate_spaces_functor
{
	std::vector<XrSpaceLocationData> locations;
	std::vector<XrSpaceVelocityData> velocities;
	std::vector<wivrn::device_id> devices;
	std::vector<XrSpace> spaces;
	XrSpace reference;
	PFN_xrLocateSpaces locate_spaces = nullptr;

public:
	locate_spaces_functor(xr::instance & instance, XrSpace reference) :
	        reference(reference)
	{
		try
		{
			if (instance.get_api_version() >= XR_MAKE_VERSION(1, 1, 0))
				locate_spaces = instance.get_proc<PFN_xrLocateSpaces>("xrLocateSpaces");
			else
				locate_spaces = instance.get_proc<PFN_xrLocateSpacesKHR>("xrLocateSpacesKHR");
		}
		catch (std::exception & e)
		{
			spdlog::warn("Failed to load xrLocateSpaces function, fallback to xrLocateSpace");
		}
	}

	void add_space(wivrn::device_id device, XrSpace space, XrTime t, std::vector<from_headset::tracking::pose> & out)
	{
		if (locate_spaces)
		{
			// store, will be located later
			devices.push_back(device);
			spaces.push_back(space);
		}
		else
			out.push_back(locate_space(device, space, reference, t));
	}

	void resolve(
	        xr::session & session,
	        XrTime t,
	        std::vector<from_headset::tracking::pose> & out)
	{
		assert(devices.size() == spaces.size());
		if (devices.empty())
			return;
		if (locate_spaces)
		{
			locations.resize(spaces.size());
			velocities.resize(spaces.size());
			XrSpaceVelocities spc_velocities{
			        .type = XR_TYPE_SPACE_VELOCITIES,
			        .velocityCount = uint32_t(velocities.size()),
			        .velocities = velocities.data(),
			};
			XrSpaceLocations spc_locations{
			        .type = XR_TYPE_SPACE_LOCATIONS,
			        .next = &spc_velocities,
			        .locationCount = uint32_t(locations.size()),
			        .locations = locations.data(),
			};
			XrSpacesLocateInfo info{
			        .type = XR_TYPE_SPACES_LOCATE_INFO,
			        .baseSpace = reference,
			        .time = t,
			        .spaceCount = uint32_t(spaces.size()),
			        .spaces = spaces.data(),
			};
			auto res = locate_spaces(session, &info, &spc_locations);
			if (XR_SUCCEEDED(res))
			{
				for (size_t i = 0; i < devices.size(); ++i)
				{
					const auto & location = locations[i];
					const auto & velocity = velocities[i];

					out.push_back({
					        .pose = location.pose,
					        .linear_velocity = velocity.linearVelocity,
					        .angular_velocity = velocity.angularVelocity,
					        .device = devices[i],
					        .flags = from_headset::to_pose_flags(location.locationFlags, velocity.velocityFlags),
					});
				}
			}
			else
				spdlog::warn("xrLocateSpaces failed: {}", xr::to_string(res));
			devices.clear();
			spaces.clear();
		}
	}
};

} // namespace

static std::optional<std::array<from_headset::hand_tracking::pose, XR_HAND_JOINT_COUNT_EXT>> locate_hands(xr::hand_tracker & hand, XrSpace space, XrTime time)
{
	auto located = hand.locate(space, time);

	if (located and located->is_input_source())
	{
		std::array<from_headset::hand_tracking::pose, XR_HAND_JOINT_COUNT_EXT> poses;
		for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
		{
			const auto & joint = located->joints[i];
			poses[i] = {
			        .position = joint.first.pose.position,
			        .orientation = pack(joint.first.pose.orientation),
			        .linear_velocity = joint.second.linearVelocity,
			        .angular_velocity = joint.second.angularVelocity,
			        .radius = uint16_t(joint.first.radius * 10'000),
			        .flags = from_headset::to_pose_flags(joint.first.locationFlags, joint.second.velocityFlags),
			};
		}

		return poses;
	}
	else
		return std::nullopt;
}

template <typename T>
static T & from_pool(std::vector<T> & container, std::vector<T> & pool)
{
	if (pool.empty())
		return container.emplace_back();
	auto & result = container.emplace_back(std::move(pool.back()));
	pool.pop_back();
	return result;
}

static xr::spaces device_to_space(device_id id)
{
	switch (id)
	{
		case device_id::HEAD:
			return xr::spaces::view;
		case device_id::EYE_GAZE:
			return xr::spaces::eye_gaze;
		case device_id::LEFT_AIM:
			return xr::spaces::aim_left;
		case device_id::LEFT_GRIP:
			return xr::spaces::grip_left;
		case device_id::LEFT_PALM:
			return xr::spaces::palm_left;
		case device_id::LEFT_PINCH_POSE:
			return xr::spaces::pinch_left;
		case device_id::LEFT_POKE:
			return xr::spaces::poke_left;
		case device_id::RIGHT_AIM:
			return xr::spaces::aim_right;
		case device_id::RIGHT_GRIP:
			return xr::spaces::grip_right;
		case device_id::RIGHT_PALM:
			return xr::spaces::palm_right;
		case device_id::RIGHT_PINCH_POSE:
			return xr::spaces::pinch_right;
		case device_id::RIGHT_POKE:
			return xr::spaces::poke_right;
		default:
			assert(false);
			__builtin_unreachable();
	}
}

void scenes::stream::tracking()
{
#ifdef __ANDROID__
	// Runtime may use JNI and needs the thread to be attached
	application::instance().setup_jni();

	XrTime next_battery_check = 0;
	const XrDuration battery_check_interval = 30'000'000'000; // 30s
#endif

	magic_enum::containers::array<device_id, XrSpace> spaces{};

	const auto & config = application::get_config();

	{
		std::vector ids{
		        device_id::HEAD,
		        device_id::LEFT_AIM,
		        device_id::LEFT_GRIP,
		        device_id::LEFT_PALM,
		        device_id::RIGHT_AIM,
		        device_id::RIGHT_GRIP,
		        device_id::RIGHT_PALM,
		};
		if (instance.has_extension(XR_EXT_HAND_INTERACTION_EXTENSION_NAME))
		{
			spdlog::info("Adding hand_interaction poses to device list");
			ids.insert(ids.end(), {device_id::LEFT_PINCH_POSE, device_id::LEFT_POKE, device_id::RIGHT_PINCH_POSE, device_id::RIGHT_POKE});
		}

		if (config.check_feature(feature::eye_gaze))
			ids.emplace_back(device_id::EYE_GAZE);

		for (auto id: ids)
		{
			if (XrSpace space = application::space(device_to_space(id)))
				spaces[id] = space;
			else
				spdlog::warn("Missing space for device {}", magic_enum::enum_name(id));
		}
	}

	XrSpace view_space = application::space(xr::spaces::view);

	// poses sent to the PC are located against this space instead of xr::spaces::world
	// directly, so the configured player height offset applies uniformly to the head, hands
	// and body. It shares xr::spaces::world's STAGE origin, translated by -offset: locating a
	// pose in it therefore reports that pose offset up by +offset, i.e. increases perceived
	// height.
	auto make_height_offset_space = [&](float offset) {
		return session.create_reference_space(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {0, -offset, 0}});
	};
	float applied_height_offset = config.get_height_offset();
	xr::space height_offset_space = make_height_offset_space(applied_height_offset);

	XrTime t0 = instance.now();
	from_headset::tracking tracking;
	std::vector<from_headset::hand_tracking> hands;
	std::vector<std::variant<from_headset::meta_body, from_headset::bd_body, from_headset::htc_body>> body;
	std::vector<XrView> views;

	std::vector<serialization_packet> packets;

	const bool hand_tracking = config.check_feature(feature::hand_tracking);
	std::optional<xr::hand_tracker> left_hand;
	std::optional<xr::hand_tracker> right_hand;

	const bool face_tracking = config.check_feature(feature::face_tracking);
	xr::face_tracker face_tracker;

	const bool body_tracking = config.check_feature(feature::body_tracking);
	xr::body_tracker body_tracker;

	locate_spaces_functor locate_spaces{instance, height_offset_space};

	on_interaction_profile_changed({});

	decltype(to_headset::tracking_control::pattern) pattern;
	size_t pattern_position = 0;

	XrDuration frame_duration{};
	XrTime pattern_begin = instance.now();
	frame_gaze_stabilizer gaze_stabilizer;
	steam_frame_eye_mmap frame_eye_mmap(frame_gaze_fix_enabled());
	std::optional<XrDuration> gaze_canonical_prediction_ns;

	if (frame_gaze_fix_enabled())
	{
		spdlog::info(
		        "Steam Frame gaze v5 active: binocular mmap source, 2.75 deg fixation lock, 5.00 deg saccade");
	}

	while (state_ != state::shutdown)
	{
		try
		{
			if (float offset = config.get_height_offset(); offset != applied_height_offset)
			{
				applied_height_offset = offset;
				height_offset_space = make_height_offset_space(applied_height_offset);
				locate_spaces = locate_spaces_functor{instance, height_offset_space};
			}

			if (pattern_position == pattern.size())
			{
				// Upper limit to 200FPS
				frame_duration = std::max<XrDuration>(5'000'000, display_time_period);
				// Wait for next frame
				XrTime now = instance.now();
				pattern_begin = display_time_phase + (std::max(pattern_begin + frame_duration, now) / frame_duration) * frame_duration;
				std::this_thread::sleep_for(std::chrono::nanoseconds(pattern_begin - now));

				pattern_position = 0;
				// Check if a new pattern has been received
				if (auto locked = tracking_control.lock(); pattern.empty() or not locked->pattern.empty())
				{
					pattern.clear();
					std::swap(pattern, locked->pattern);
					pattern_position = 0;

					// Ensure head tracking is always done
					if (not std::ranges::contains(pattern, device_id::HEAD, &to_headset::tracking_control::sample::device))
						pattern.push_back({.device = device_id::HEAD});

					if (hand_tracking)
					{
						if (std::ranges::contains(pattern, device_id::LEFT_HAND, &to_headset::tracking_control::sample::device))
						{
							if (not left_hand)
								left_hand = session.create_hand_tracker(XR_HAND_LEFT_EXT);
						}
						else
							left_hand.reset();

						if (std::ranges::contains(pattern, device_id::RIGHT_HAND, &to_headset::tracking_control::sample::device))
						{
							if (not right_hand)
								right_hand = session.create_hand_tracker(XR_HAND_RIGHT_EXT);
						}
						else
							right_hand.reset();
					}

					if (face_tracking)
					{
						if (std::ranges::contains(pattern, device_id::FACE, &to_headset::tracking_control::sample::device))
						{
							if (std::holds_alternative<std::monostate>(face_tracker))
								face_tracker = xr::make_face_tracker(instance, system, session);
						}
						else
							face_tracker.emplace<std::monostate>();
					}

					if (body_tracking)
					{
						if (std::ranges::contains(pattern, device_id::BODY, &to_headset::tracking_control::sample::device))
						{
							if (std::holds_alternative<std::monostate>(body_tracker))
								body_tracker = xr::make_body_tracker(
								        instance,
								        system,
								        session,
								        application::get_generic_trackers());
						}
						else
							body_tracker.emplace<std::monostate>();
					}

					gaze_canonical_prediction_ns.reset();
					for (const auto & item: pattern)
					{
						if (item.device != device_id::EYE_GAZE)
						{
							continue;
						}

						if (not gaze_canonical_prediction_ns or
						    std::abs(item.prediction_ns) < std::abs(*gaze_canonical_prediction_ns))
						{
							gaze_canonical_prediction_ns = item.prediction_ns;
						}
					}

					std::ranges::sort(pattern, std::less{}, [frame_duration](const auto & i) { return -i.prediction_ns % frame_duration; });

					spdlog::info("Tracking pattern ({}µs):", frame_duration / 1'000);
					for (const auto & [device, pred]: pattern)
					{
						spdlog::info("\tt+{}µs {} ({}µs)",
						             (frame_duration - (pred % frame_duration)) / 1'000,
						             magic_enum::enum_name(device),
						             pred / 1'000);
					}
				}
			}

			hands.clear();
			body.clear();

			XrTime now = instance.now();
			XrTime t0 = pattern_begin + frame_duration - (pattern[pattern_position].prediction_ns % frame_duration);
			if (t0 > now + 500)
				std::this_thread::sleep_for(std::chrono::nanoseconds(t0 - now));

			bool interaction_profile_changed = this->interaction_profile_changed.exchange(false);

			if (interaction_profile_changed)
				if (auto htc = std::get_if<xr::htc_body_tracker>(&body_tracker))
					htc->update_active();

			tracking.interaction_profiles = {
			        interaction_profiles[0].load(),
			        interaction_profiles[1].load(),
			        interaction_profiles[2].load(),
			};

			tracking.production_timestamp = t0;
			tracking.timestamp = t0 + pattern[pattern_position].prediction_ns;
			tracking.view_flags = {};
			tracking.state_flags = {};
			tracking.views = {};
			tracking.device_poses.clear();
			tracking.face = {};

			if (recenter_requested.exchange(false))
				tracking.state_flags = wivrn::from_headset::tracking::recentered;

			try
			{
				for (; pattern_position < pattern.size(); ++pattern_position)
				{
					const auto & item = pattern[pattern_position];
					auto at_time = t0 + item.prediction_ns;

					// Don't merge items that are too far from the current one
					if (std::abs(tracking.timestamp - at_time) > 1'000'000)
						break;

					switch (item.device)
					{
						case device_id::HEAD:
							tracking.view_flags = session.locate_views(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, tracking.timestamp, view_space, views);
							assert(views.size() == tracking.views.size());
							for (auto [i, j]: std::views::zip(views, tracking.views))
							{
								j.pose = i.pose;
								j.fov = i.fov;
							}
							locate_spaces.add_space(item.device, view_space, tracking.timestamp, tracking.device_poses);
							break;
						case wivrn::device_id::LEFT_GRIP:
						case wivrn::device_id::LEFT_AIM:
						case wivrn::device_id::LEFT_PALM:
						case wivrn::device_id::RIGHT_GRIP:
						case wivrn::device_id::RIGHT_AIM:
						case wivrn::device_id::RIGHT_PALM:
						case wivrn::device_id::LEFT_PINCH_POSE:
						case wivrn::device_id::LEFT_POKE:
						case wivrn::device_id::RIGHT_PINCH_POSE:
						case wivrn::device_id::RIGHT_POKE:
							locate_spaces.add_space(item.device, spaces[item.device], tracking.timestamp, tracking.device_poses);
							break;
						case wivrn::device_id::EYE_GAZE: {
							bool used_frame_mmap = false;

							if (frame_gaze_fix_enabled() and frame_eye_mmap.active())
							{
								if (auto mmap_sample = frame_eye_mmap.read())
								{
									const glm::quat gaze_quat =
									        steam_frame_eye_mmap::direction_to_quaternion(mmap_sample->direction);
									using flags = from_headset::pose_flags;

									tracking.device_poses.push_back(
									        from_headset::tracking::pose{
									                .pose = {
									                        .orientation = {
									                                .x = gaze_quat.x,
									                                .y = gaze_quat.y,
									                                .z = gaze_quat.z,
									                                .w = gaze_quat.w,
									                        },
									                },
									                .device = item.device,
									                .flags = uint8_t(flags::orientation_valid) | uint8_t(flags::orientation_tracked),
									        });

									gaze_stabilizer.apply(
									        tracking.device_poses.back(),
									        tracking.timestamp,
									        XrTime(mmap_sample->sequence),
									        true);
									used_frame_mmap = true;
								}
							}

							if (not used_frame_mmap)
							{
								if (application::get_hmd_traits().view_locate and not frame_gaze_fix_enabled())
								{
									tracking.device_poses.push_back(
									        locate_space(
									                item.device,
									                spaces[item.device],
									                spaces[wivrn::device_id::HEAD],
									                tracking.timestamp));
								}
								else
								{
									auto gaze =
									        locate_space(
									                item.device,
									                spaces[item.device],
									                height_offset_space,
									                tracking.timestamp);
									auto view_pose =
									        locate_space(
									                item.device,
									                view_space,
									                height_offset_space,
									                tracking.timestamp);

									glm::quat gaze_quat(
									        gaze.pose.orientation.w,
									        gaze.pose.orientation.x,
									        gaze.pose.orientation.y,
									        gaze.pose.orientation.z);
									glm::quat view_quat(
									        view_pose.pose.orientation.w,
									        view_pose.pose.orientation.x,
									        view_pose.pose.orientation.y,
									        view_pose.pose.orientation.z);
									gaze_quat = glm::conjugate(view_quat) * gaze_quat;

									using flags = from_headset::pose_flags;
									tracking.device_poses.push_back(
									        from_headset::tracking::pose{
									                .pose = {
									                        .orientation = {
									                                .x = gaze_quat.x,
									                                .y = gaze_quat.y,
									                                .z = gaze_quat.z,
									                                .w = gaze_quat.w,
									                        },
									                },
									                .device = item.device,
									                .flags = uint8_t(gaze.flags & view_pose.flags & ~(flags::linear_velocity_valid | flags::angular_velocity_valid)),
									        });
								}

								if (frame_gaze_fix_enabled() and
								    not tracking.device_poses.empty() and
								    tracking.device_poses.back().device == wivrn::device_id::EYE_GAZE)
								{
									const bool canonical_sample =
									        gaze_canonical_prediction_ns and
									        item.prediction_ns == *gaze_canonical_prediction_ns;

									gaze_stabilizer.apply(
									        tracking.device_poses.back(),
									        tracking.timestamp,
									        pattern_begin,
									        canonical_sample);
								}
							}
							break;
						}
						case wivrn::device_id::FACE:
							std::visit(utils::overloaded{
							                   [](std::monostate &) {},
							                   [&](auto & ft) {
								                   ft.get_weights(at_time, tracking.face.emplace<typename std::remove_reference_t<decltype(ft)>::packet_type>());
							                   },
							           },
							           face_tracker);
							break;
						case wivrn::device_id::LEFT_HAND:
							if (left_hand)
							{
								hands.emplace_back(
								        t0,
								        at_time,
								        from_headset::hand_tracking::left,
								        locate_hands(*left_hand, height_offset_space, tracking.timestamp));
							}
							break;
						case wivrn::device_id::RIGHT_HAND:
							if (right_hand)
							{
								hands.emplace_back(
								        t0,
								        at_time,
								        from_headset::hand_tracking::right,
								        locate_hands(*right_hand, height_offset_space, tracking.timestamp));
							}
							break;
						case wivrn::device_id::BODY:
							std::visit(utils::overloaded{
							                   [](std::monostate &) {},
							                   [&](auto & b) {
								                   auto packet = b.locate_spaces(at_time, height_offset_space);
								                   packet.timestamp = at_time;
								                   packet.production_timestamp = tracking.production_timestamp;
								                   body.push_back(packet);
							                   },
							           },
							           body_tracker);
							break;
						default:
							break;
					}
				}
				locate_spaces.resolve(session, tracking.timestamp, tracking.device_poses);
			}
			catch (const std::system_error & e)
			{
				if (e.code().category() != xr::error_category() or
				    e.code().value() != XR_ERROR_TIME_INVALID)
					throw;
			}

#ifdef __ANDROID__
			// FIXME: switch to event based
			if (next_battery_check < now)
			{
				auto status = get_battery_status();
				network_session->send_stream(from_headset::battery{
				        .charge = status.charge.value_or(-1),
				        .present = status.charge.has_value(),
				        .charging = status.charging,
				});

				next_battery_check = now + battery_check_interval;
			}
#endif

			if (auto fb = std::get_if<xr::fb_body_tracker>(&body_tracker); fb and fb->should_send_skeleton())
			{
				try
				{
					network_session->send_control(fb->get_skeleton());
				}
				catch (std::exception & e)
				{
					spdlog::warn("Failed to send body skeleton: {}", e.what());
				}
			}

			packets.resize(std::max(packets.size(), 1 + hands.size() + body.size()));
			size_t packet_count = 0;

			if (not(tracking.device_poses.empty() and std::holds_alternative<std::monostate>(tracking.face)))
			{
				auto & packet = packets[packet_count++];
				packet.clear();
				wivrn_session::stream_socket_t::serialize(packet, tracking);
			}

			for (const auto & i: hands)
			{
				auto & packet = packets[packet_count++];
				packet.clear();
				wivrn_session::stream_socket_t::serialize(packet, i);
			}
			for (const auto & i: body)
			{
				auto & packet = packets[packet_count++];
				packet.clear();
				std::visit(utils::overloaded{
				                   [&](auto & p) {
					                   wivrn_session::stream_socket_t::serialize(packet, p);
				                   },
				           },
				           i);
			}

			network_session->send_stream(std::span(packets.data(), packet_count));

			XrTime old = scheduled_derived_pose;
			if (old and now > old)
			{
				send_derived_pose();
				scheduled_derived_pose.compare_exchange_strong(old, 0);
			}
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in tracking thread, exiting: {}", e.what());
			exit();
		}
	}
}

void scenes::stream::operator()(to_headset::tracking_control && packet)
{
	*tracking_control.lock() = std::move(packet);
}

static device_id derived_from(device_id target)
{
	switch (target)
	{
		case device_id::LEFT_AIM:
		case device_id::LEFT_PALM:
		case device_id::LEFT_PINCH_POSE:
		case device_id::LEFT_POKE:
			return device_id::LEFT_GRIP;
		case device_id::RIGHT_AIM:
		case device_id::RIGHT_PALM:
		case device_id::RIGHT_PINCH_POSE:
		case device_id::RIGHT_POKE:
			return device_id::RIGHT_GRIP;
		default:
			assert(false);
			__builtin_unreachable();
	}
}

void scenes::stream::on_interaction_profile_changed(const XrEventDataInteractionProfileChanged &)
{
	interaction_profile_changed = true;
	std::array path = {
	        "/user/hand/left",
	        "/user/hand/right",
	        "/user/gamepad",
	};
#define DO_PROFILE(vendor, name)                                                \
	if (profile == "/interaction_profiles/" #vendor "/" #name)              \
	{                                                                       \
		interaction_profiles[i] = interaction_profile::vendor##_##name; \
		continue;                                                       \
	}

	for (size_t i = 0; i < path.size(); ++i)
	{
		try
		{
			auto profile = session.get_current_interaction_profile(path[i]);
			spdlog::info("interaction profile for {}: {}", path[i], profile);
			DO_PROFILE(khr, simple_controller)
			DO_PROFILE(ext, hand_interaction_ext)
			DO_PROFILE(bytedance, pico_neo3_controller)
			DO_PROFILE(bytedance, pico4_controller)
			DO_PROFILE(bytedance, pico4s_controller)
			DO_PROFILE(bytedance, pico_g3_controller)
			DO_PROFILE(google, daydream_controller)
			DO_PROFILE(hp, mixed_reality_controller)
			DO_PROFILE(htc, vive_controller)
			DO_PROFILE(htc, vive_cosmos_controller)
			DO_PROFILE(htc, vive_focus3_controller)
			DO_PROFILE(htc, vive_pro)
			DO_PROFILE(ml, ml2_controller)
			DO_PROFILE(microsoft, motion_controller)
			DO_PROFILE(microsoft, xbox_controller)
			DO_PROFILE(oculus, go_controller)
			DO_PROFILE(oculus, touch_controller)
			DO_PROFILE(meta, touch_pro_controller)
			DO_PROFILE(meta, touch_plus_controller)
			DO_PROFILE(meta, touch_controller_rift_cv1)
			DO_PROFILE(meta, touch_controller_quest_1_rift_s)
			DO_PROFILE(meta, touch_controller_quest_2)
			DO_PROFILE(yvr, touch_controller_yvr)
			DO_PROFILE(samsung, odyssey_controller)
			DO_PROFILE(valve, index_controller)
			DO_PROFILE(valve, frame_controller_valve)

			// FIXME: remove once support for pre-1.1 profiles is dropped
			if (profile == "/interaction_profiles/facebook/touch_controller_pro")
			{
				interaction_profiles[i] = interaction_profile::meta_touch_pro_controller;
				continue;
			}
			if (profile == "/interaction_profiles/meta/touch_controller_plus")
			{
				interaction_profiles[i] = interaction_profile::meta_touch_plus_controller;
				continue;
			}
			spdlog::warn("unknown interaction profile {}", profile);
		}
		catch (std::exception & e)
		{
			spdlog::warn("Failed to get current interaction profile: {}", e.what());
		}
		interaction_profiles[i] = interaction_profile::none;
	}

	// Wait for runtime to know about the controllers before sending data
	scheduled_derived_pose = instance.now() + 1'000'000'000;
}

void scenes::stream::send_derived_pose()
{
	auto now = instance.now();
	for (device_id target: {
	             device_id::LEFT_AIM,
	             device_id::LEFT_PALM,
	             device_id::LEFT_PINCH_POSE,
	             device_id::LEFT_POKE,
	             device_id::RIGHT_AIM,
	             device_id::RIGHT_PALM,
	             device_id::RIGHT_PINCH_POSE,
	             device_id::RIGHT_POKE,
	     })
	{
		// don't do derived poses for hand interaction
		const bool right = (target >= device_id::RIGHT_GRIP && target <= device_id::RIGHT_PALM) || target == device_id::RIGHT_PINCH_POSE || target == device_id::RIGHT_POKE;
		if (interaction_profiles[right] == interaction_profile::ext_hand_interaction_ext)
			continue;

		auto source = derived_from(target);
		auto source_space = application::space(device_to_space(source));
		auto target_space = application::space(device_to_space(target));

		if (not(source_space and target_space))
		{
			// This may happen if the runtime does not support palm ext
			// check if we have a device specific offset
			if (not application::get_hmd_traits().hand_interaction_grip_surface)
			{
				switch (target)
				{
					case device_id::LEFT_PALM:
					case device_id::RIGHT_PALM: {
						glm::quat q(glm::vec3(glm::radians(-60.), 0, 0));
						network_session->send_control(from_headset::derived_pose{
						        .source = source,
						        .target = target,
						        .relation = {
						                .orientation = {
						                        .x = q.x,
						                        .y = q.y,
						                        .z = q.z,
						                        .w = q.w,
						                },
						        },
						});
					}
					break;
					default:
						break;
				}
			}
		}
		else
		{
			if (auto pose = locate_space(target, target_space, source_space, now);
			    pose.flags & from_headset::pose_flags::position_valid and pose.flags & from_headset::pose_flags::orientation_valid)
			{
				network_session->send_control(from_headset::derived_pose{
				        .source = source,
				        .target = target,
				        .relation = pose.pose,
				});
			}
			else
			{
				// source == target means that the pose cannot be derived
				network_session->send_control(from_headset::derived_pose{
				        .source = target,
				        .target = target,
				});
			}
		}
	}
}
