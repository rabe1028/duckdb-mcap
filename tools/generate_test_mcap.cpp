// Standalone tool to generate a test MCAP file with proper CDR-encoded messages
// Simulates a minimal Autoware-like recording with realistic ROS2 schemas

#define MCAP_IMPLEMENTATION
#define MCAP_COMPRESSION_NO_LZ4
#define MCAP_COMPRESSION_NO_ZSTD

#include <mcap/writer.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ── CDR Writer ─────────────────────────────────────────────────────────────
// Produces CDR_LE (little-endian) encoded data with proper encapsulation header.
class CdrWriter {
public:
	CdrWriter() {
		// CDR_LE encapsulation header
		buf_.push_back(0x00); // reserved
		buf_.push_back(0x01); // CDR_LE
		buf_.push_back(0x00); // options
		buf_.push_back(0x00); // options
	}

	void WriteBool(bool v) {
		buf_.push_back(v ? 1 : 0);
	}
	void WriteUint8(uint8_t v) {
		buf_.push_back(v);
	}
	void WriteInt32(int32_t v) {
		AlignTo(4);
		AppendLE(v);
	}
	void WriteUint32(uint32_t v) {
		AlignTo(4);
		AppendLE(v);
	}
	void WriteFloat32(float v) {
		AlignTo(4);
		AppendLE(v);
	}
	void WriteFloat64(double v) {
		AlignTo(8);
		AppendLE(v);
	}
	void WriteString(const std::string &s) {
		WriteUint32(static_cast<uint32_t>(s.size() + 1)); // length including null
		for (char c : s)
			buf_.push_back(static_cast<uint8_t>(c));
		buf_.push_back(0); // null terminator
	}

	// Write a dynamic array header (element count)
	void WriteSequenceLength(uint32_t count) {
		WriteUint32(count);
	}

	const std::byte *Data() const {
		return reinterpret_cast<const std::byte *>(buf_.data());
	}
	size_t Size() const {
		return buf_.size();
	}

private:
	void AlignTo(size_t alignment) {
		size_t data_offset = buf_.size() - 4; // offset relative to after encap header
		size_t rem = data_offset % alignment;
		if (rem != 0) {
			size_t pad = alignment - rem;
			for (size_t i = 0; i < pad; i++)
				buf_.push_back(0);
		}
	}

	template <typename T>
	void AppendLE(T v) {
		auto *bytes = reinterpret_cast<const uint8_t *>(&v);
		for (size_t i = 0; i < sizeof(T); i++)
			buf_.push_back(bytes[i]);
	}

	std::vector<uint8_t> buf_;
};

// ── Schema Definitions ─────────────────────────────────────────────────────
// Real ROS2 message schemas with nested type dependencies

static const char *POSE_STAMPED_SCHEMA =
    "std_msgs/msg/Header header\n"
    "geometry_msgs/msg/Pose pose\n"
    "================================================================================\n"
    "MSG: std_msgs/msg/Header\n"
    "builtin_interfaces/msg/Time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: builtin_interfaces/msg/Time\n"
    "int32 sec\n"
    "uint32 nanosec\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Pose\n"
    "geometry_msgs/msg/Point position\n"
    "geometry_msgs/msg/Quaternion orientation\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Point\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Quaternion\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n"
    "float64 w\n";

static const char *IMU_SCHEMA =
    "std_msgs/msg/Header header\n"
    "geometry_msgs/msg/Quaternion orientation\n"
    "float64[9] orientation_covariance\n"
    "geometry_msgs/msg/Vector3 angular_velocity\n"
    "float64[9] angular_velocity_covariance\n"
    "geometry_msgs/msg/Vector3 linear_acceleration\n"
    "float64[9] linear_acceleration_covariance\n"
    "================================================================================\n"
    "MSG: std_msgs/msg/Header\n"
    "builtin_interfaces/msg/Time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: builtin_interfaces/msg/Time\n"
    "int32 sec\n"
    "uint32 nanosec\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Quaternion\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n"
    "float64 w\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Vector3\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n";

static const char *TWIST_STAMPED_SCHEMA =
    "std_msgs/msg/Header header\n"
    "geometry_msgs/msg/Twist twist\n"
    "================================================================================\n"
    "MSG: std_msgs/msg/Header\n"
    "builtin_interfaces/msg/Time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: builtin_interfaces/msg/Time\n"
    "int32 sec\n"
    "uint32 nanosec\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Twist\n"
    "geometry_msgs/msg/Vector3 linear\n"
    "geometry_msgs/msg/Vector3 angular\n"
    "================================================================================\n"
    "MSG: geometry_msgs/msg/Vector3\n"
    "float64 x\n"
    "float64 y\n"
    "float64 z\n";

static const char *FLOAT32_SCHEMA = "float32 data\n";

static const char *POINTCLOUD2_SCHEMA =
    "std_msgs/msg/Header header\n"
    "uint32 height\n"
    "uint32 width\n"
    "sensor_msgs/msg/PointField[] fields\n"
    "bool is_bigendian\n"
    "uint32 point_step\n"
    "uint32 row_step\n"
    "uint8[] data\n"
    "bool is_dense\n"
    "================================================================================\n"
    "MSG: std_msgs/msg/Header\n"
    "builtin_interfaces/msg/Time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: builtin_interfaces/msg/Time\n"
    "int32 sec\n"
    "uint32 nanosec\n"
    "================================================================================\n"
    "MSG: sensor_msgs/msg/PointField\n"
    "string name\n"
    "uint32 offset\n"
    "uint8 datatype\n"
    "uint32 count\n";

// ── CDR Encoding Helpers ───────────────────────────────────────────────────

// Write a std_msgs/msg/Header
static void WriteHeader(CdrWriter &w, int32_t sec, uint32_t nanosec, const std::string &frame_id) {
	// builtin_interfaces/msg/Time stamp
	w.WriteInt32(sec);
	w.WriteUint32(nanosec);
	// string frame_id
	w.WriteString(frame_id);
}

// Write geometry_msgs/msg/Point
static void WritePoint(CdrWriter &w, double x, double y, double z) {
	w.WriteFloat64(x);
	w.WriteFloat64(y);
	w.WriteFloat64(z);
}

// Write geometry_msgs/msg/Quaternion
static void WriteQuaternion(CdrWriter &w, double x, double y, double z, double qw) {
	w.WriteFloat64(x);
	w.WriteFloat64(y);
	w.WriteFloat64(z);
	w.WriteFloat64(qw);
}

// Write geometry_msgs/msg/Vector3
static void WriteVector3(CdrWriter &w, double x, double y, double z) {
	w.WriteFloat64(x);
	w.WriteFloat64(y);
	w.WriteFloat64(z);
}

int main(int argc, char *argv[]) {
	const std::string output_path = (argc > 1) ? argv[1] : "data/demo.mcap";

	mcap::McapWriter writer;
	mcap::McapWriterOptions options("ros2");
	options.compression = mcap::Compression::None;

	auto status = writer.open(output_path, options);
	if (!status.ok()) {
		std::cerr << "Failed to open " << output_path << ": " << status.message << "\n";
		return 1;
	}

	// ── Schemas ──
	mcap::Schema pose_schema("geometry_msgs/msg/PoseStamped", "ros2msg", POSE_STAMPED_SCHEMA);
	writer.addSchema(pose_schema);

	mcap::Schema imu_schema("sensor_msgs/msg/Imu", "ros2msg", IMU_SCHEMA);
	writer.addSchema(imu_schema);

	mcap::Schema twist_schema("geometry_msgs/msg/TwistStamped", "ros2msg", TWIST_STAMPED_SCHEMA);
	writer.addSchema(twist_schema);

	mcap::Schema float_schema("std_msgs/msg/Float32", "ros2msg", FLOAT32_SCHEMA);
	writer.addSchema(float_schema);

	mcap::Schema pc2_schema("sensor_msgs/msg/PointCloud2", "ros2msg", POINTCLOUD2_SCHEMA);
	writer.addSchema(pc2_schema);

	// ── Channels ──
	mcap::Channel pose_ch("/localization/pose", "cdr", pose_schema.id);
	writer.addChannel(pose_ch);

	mcap::Channel imu_ch("/sensing/imu/data", "cdr", imu_schema.id);
	writer.addChannel(imu_ch);

	mcap::Channel twist_ch("/vehicle/status/twist", "cdr", twist_schema.id);
	writer.addChannel(twist_ch);

	mcap::Channel velocity_ch("/vehicle/status/velocity", "cdr", float_schema.id);
	writer.addChannel(velocity_ch);

	mcap::Channel lidar_ch("/sensing/lidar/pointcloud", "cdr", pc2_schema.id);
	writer.addChannel(lidar_ch);

	// ── Generate messages ──
	// Base: 2024-01-15 10:00:00 UTC
	const int32_t base_sec = 1705312800;
	const uint64_t base_ns = static_cast<uint64_t>(base_sec) * 1000000000ULL;
	uint32_t seq = 0;

	for (int i = 0; i < 100; i++) {
		uint64_t t = base_ns + static_cast<uint64_t>(i) * 100000000ULL; // 100ms = 10Hz
		int32_t sec = static_cast<int32_t>(t / 1000000000ULL);
		uint32_t nsec = static_cast<uint32_t>(t % 1000000000ULL);

		// ── PoseStamped (10 Hz) ──
		{
			CdrWriter w;
			WriteHeader(w, sec, nsec, "map");
			// Pose: position + orientation
			double x = 100.0 + i * 0.5;
			double y = 200.0 + std::sin(i * 0.1) * 10.0;
			double z = 0.0;
			WritePoint(w, x, y, z);
			WriteQuaternion(w, 0.0, 0.0, std::sin(i * 0.05), std::cos(i * 0.05));

			mcap::Message msg;
			msg.channelId = pose_ch.id;
			msg.sequence = seq++;
			msg.logTime = t;
			msg.publishTime = t;
			msg.data = w.Data();
			msg.dataSize = w.Size();
			writer.write(msg);
		}

		// ── IMU (10 Hz) ──
		{
			CdrWriter w;
			WriteHeader(w, sec, nsec, "imu_link");
			// orientation (quaternion)
			WriteQuaternion(w, 0.0, 0.0, std::sin(i * 0.02), std::cos(i * 0.02));
			// orientation_covariance [9]
			for (int j = 0; j < 9; j++)
				w.WriteFloat64(j % 4 == 0 ? 0.001 : 0.0);
			// angular_velocity (Vector3)
			WriteVector3(w, 0.001, -0.002, 0.0003 * std::sin(i * 0.5));
			// angular_velocity_covariance [9]
			for (int j = 0; j < 9; j++)
				w.WriteFloat64(j % 4 == 0 ? 0.0001 : 0.0);
			// linear_acceleration (Vector3)
			WriteVector3(w, 0.01 * std::sin(i * 0.2), 0.02 * std::cos(i * 0.3), 9.81);
			// linear_acceleration_covariance [9]
			for (int j = 0; j < 9; j++)
				w.WriteFloat64(j % 4 == 0 ? 0.01 : 0.0);

			mcap::Message msg;
			msg.channelId = imu_ch.id;
			msg.sequence = seq++;
			msg.logTime = t;
			msg.publishTime = t;
			msg.data = w.Data();
			msg.dataSize = w.Size();
			writer.write(msg);
		}

		// ── TwistStamped (10 Hz) ──
		{
			CdrWriter w;
			WriteHeader(w, sec, nsec, "base_link");
			// Twist: linear + angular
			double vx = 10.0 + i * 0.1;
			WriteVector3(w, vx, 0.0, 0.0);
			WriteVector3(w, 0.0, 0.0, 0.01 * std::sin(i * 0.1));

			mcap::Message msg;
			msg.channelId = twist_ch.id;
			msg.sequence = seq++;
			msg.logTime = t;
			msg.publishTime = t;
			msg.data = w.Data();
			msg.dataSize = w.Size();
			writer.write(msg);
		}

		// ── Velocity Float32 (5 Hz) ──
		if (i % 2 == 0) {
			CdrWriter w;
			w.WriteFloat32(static_cast<float>(10.0 + i * 0.1));

			mcap::Message msg;
			msg.channelId = velocity_ch.id;
			msg.sequence = seq++;
			msg.logTime = t;
			msg.publishTime = t;
			msg.data = w.Data();
			msg.dataSize = w.Size();
			writer.write(msg);
		}

		// ── PointCloud2 (10 Hz) ──
		{
			CdrWriter w;
			WriteHeader(w, sec, nsec, "velodyne");
			// height, width
			w.WriteUint32(1);
			w.WriteUint32(100 + i);
			// fields[] (dynamic array of PointField)
			w.WriteSequenceLength(3); // 3 fields: x, y, z
			// PointField: name(string), offset(uint32), datatype(uint8), count(uint32)
			// x
			w.WriteString("x");
			w.WriteUint32(0);
			w.WriteUint8(7); // FLOAT32
			w.WriteUint32(1);
			// y
			w.WriteString("y");
			w.WriteUint32(4);
			w.WriteUint8(7);
			w.WriteUint32(1);
			// z
			w.WriteString("z");
			w.WriteUint32(8);
			w.WriteUint8(7);
			w.WriteUint32(1);
			// is_bigendian
			w.WriteBool(false);
			// point_step
			w.WriteUint32(12);
			// row_step
			w.WriteUint32(12 * (100 + i));
			// data[] (uint8 dynamic array - point cloud binary data)
			uint32_t n_points = 100 + i;
			uint32_t data_size = n_points * 12; // 3 floats per point
			w.WriteSequenceLength(data_size);
			for (uint32_t p = 0; p < n_points; p++) {
				float px = static_cast<float>(p) * 0.1f;
				float py = std::sin(px) * 5.0f;
				float pz = 1.0f + static_cast<float>(p) * 0.01f;
				// Write raw float bytes
				auto write_float_raw = [&](float f) {
					auto *bytes = reinterpret_cast<const uint8_t *>(&f);
					for (int b = 0; b < 4; b++)
						w.WriteUint8(bytes[b]);
				};
				write_float_raw(px);
				write_float_raw(py);
				write_float_raw(pz);
			}
			// is_dense
			w.WriteBool(true);

			mcap::Message msg;
			msg.channelId = lidar_ch.id;
			msg.sequence = seq++;
			msg.logTime = t;
			msg.publishTime = t - 1000000; // 1ms earlier (sensor latency)
			msg.data = w.Data();
			msg.dataSize = w.Size();
			writer.write(msg);
		}
	}

	writer.close();
	std::cout << "Generated " << output_path << " with " << seq << " messages\n";
	return 0;
}
