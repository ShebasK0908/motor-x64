#!/bin/bash
# Compila MotorX64.app SIN firma y genera MotorX64.ipa sin firmar.
set -euo pipefail
cd "$(dirname "$0")"

xcodebuild \
  -project MotorX64.xcodeproj \
  -scheme MotorX64 \
  -configuration Release \
  -sdk iphoneos \
  -derivedDataPath build/DerivedData \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  build

bash ./package-ipa.sh \
  "build/DerivedData/Build/Products/Release-iphoneos/MotorX64.app" \
  "MotorX64.ipa"
