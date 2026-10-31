S=$(mktemp); nm -gUj "$1" 2>/dev/null | sort -u > "$S"
while read -r name re; do
  printf '%-16s %s\n' "$name" "$(grep -cE "$re" "$S")"
done <<'EOF'
zlib ^_(deflate|inflate|crc32|adler32|zlibVersion|gz[a-z]+|compress2?|uncompress2?)
abseil-cpp ^__Z[A-Z]*N?K?4absl|^_Absl
googleapis ^__Z[A-Z]*N?K?6google(3rpc|3api|4type)
googletest ^__Z[A-Z]*N?K?7testing
protobuf ^__Z[A-Z]*N?K?6google8protobuf
utf8_range ^_utf8_range_
re2 ^__Z[A-Z]*N?K?3re2
icu ^__Z[A-Z]*N?K?6icu_[0-9]|^_u[a-z]*_[A-Za-z0-9]+_[0-9]{2}$
nlohmann_json ^__Z[A-Z]*N?K?8nlohmann
EOF
rm "$S"
