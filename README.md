# duckdb-mcap

A [DuckDB](https://duckdb.org/) extension for reading [MCAP](https://mcap.dev/) files — the standard log format for robotics and autonomous driving (ROS2, Autoware, Foxglove).

Decodes CDR-serialized ROS2 messages into native DuckDB types, enabling SQL queries directly on recorded sensor data.

## Quick Start

```sql
-- List topics in the file
SELECT * FROM mcap_channels('recording.mcap');

-- Read decoded messages with native struct access
SELECT
  log_time,
  pose.position.x,
  pose.position.y,
  header.frame_id
FROM read_mcap_channel('recording.mcap', '/localization/pose');

-- IMU data
SELECT
  log_time,
  linear_acceleration.z AS accel_z,
  angular_velocity.x AS gyro_x
FROM read_mcap_channel('recording.mcap', '/sensing/imu/data');
```

## Functions

| Function | Description |
|----------|-------------|
| `read_mcap_channel('file', '/topic')` | Read messages from a topic with native type decoding |
| `mcap_channels('file')` | List all channels (topics) with schema info |
| `mcap_schemas('file')` | List message schema definitions |
| `mcap_statistics('file')` | File-level statistics (message count, time range, etc.) |

## Features

- **CDR deserialization**: ROS2 CDR binary messages decoded to native DuckDB STRUCT/LIST/BLOB types
- **Dot-notation access**: `pose.position.x`, `header.stamp.sec`, `twist.linear.x`
- **Compression**: zstd and LZ4 compressed MCAP files supported
- **ROS2 msg schema parsing**: Nested types, fixed/dynamic arrays, all primitive types
- **Type mapping**: `float64` → `DOUBLE`, `string` → `VARCHAR`, nested msgs → `STRUCT`, arrays → `LIST`, `uint8[]` → `BLOB`

## Building

### Prerequisites

```bash
# Install dev tools via mise
mise install
mise run install-dev-tools
```

Or manually install: `ninja`, `cmake`, `clang-format 11`, `black 24.x`, `cmake-format`.

### Build

```bash
git submodule update --init --recursive
make
```

Build outputs:
- `./build/release/duckdb` — DuckDB shell with extension loaded
- `./build/release/test/unittest` — test runner

### Test

```bash
make test
```

### Format

```bash
mise run format-check   # check
mise run format-fix     # auto-fix
```

## Supported Message Types

Any ROS2 message with `ros2msg` schema encoding and `cdr` message encoding is supported, including:

- `geometry_msgs/msg/PoseStamped`
- `geometry_msgs/msg/TwistStamped`
- `sensor_msgs/msg/Imu`
- `sensor_msgs/msg/PointCloud2`
- `std_msgs/msg/Float32`
- Autoware custom messages
- Any other ROS2 message type

## Third-Party Libraries

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| [foxglove/mcap](https://github.com/foxglove/mcap) | 2.1.3 | MIT | MCAP file parsing (header-only C++) |
| [lz4](https://github.com/lz4/lz4) | 1.10.0 | BSD-2-Clause | LZ4 decompression |
| zstd | DuckDB bundled | BSD-3-Clause | Zstandard decompression |

## License

MIT
