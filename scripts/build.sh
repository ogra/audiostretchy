#!/bin/bash
# this_file: scripts/build.sh

set -e

echo "🔧 AudioStretchy Build Script"
echo "============================="

# Check if we're in the right directory
if [ ! -f "pyproject.toml" ]; then
    echo "❌ Error: Please run this script from the project root directory"
    exit 1
fi

# Clean previous builds
echo "🧹 Cleaning previous builds..."
rm -rf build/ dist/ *.egg-info/

# Update submodules
echo "📥 Updating submodules..."
git submodule update --init --recursive

# Install build dependencies
echo "📦 Installing build dependencies..."
python -m pip install --upgrade pip build twine

# Compile C library before packaging
echo "🧱 Compiling C library..."
python scripts/compile_c.py --verbose

# Build the package
echo "🏗️ Building package..."
python -m build

# Verify the build
echo "✅ Verifying build..."
python -m twine check dist/*

# Display build artifacts
echo "📋 Build artifacts:"
ls -la dist/

echo "✅ Build completed successfully!"
echo "📁 Artifacts are in the 'dist/' directory"