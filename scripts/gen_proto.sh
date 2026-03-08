#!/usr/bin/env bash

set -euo pipefail

PROTO_DIR="$(cd "$(dirname "$0")/.." && pwd)/proto"
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/build/generated"

mkdir -p "$OUT_DIR"

echo "==> Generating Protobuf stubs..."
protoc -I "$PROTO_DIR" \
  --cpp_out="$OUT_DIR" \
  "$PROTO_DIR/message.proto" \
  "$PROTO_DIR/service.proto"

echo "==> Generating gRPC stubs..."
GRPC_PLUGIN="$(which grpc_cpp_plugin 2>/dev/null)"
protoc -I "$PROTO_DIR" \
  --grpc_out="$OUT_DIR" \
  --plugin="protoc-gen-grpc=$GRPC_PLUGIN" \
  "$PROTO_DIR/service.proto"

echo "==> Done. Output -> $OUT_DIR"
ls -lh "$OUT_DIR"
