/**
 * @file wolftk/threePacking.glsl
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three.js r171
 * (github.com/mrdoob/three.js, tag r171) src/renderers/shaders/ShaderChunk/packing.glsl.js: the parts of the chunk that three-geospatial's
 * shaders use (three.js prepends the chunk to every ShaderMaterial; LL does not).
 *
 * The MIT License
 *
 * Copyright © 2010-2024 three.js authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef WOLF_TK_THREEPACKING_GLSL
#define WOLF_TK_THREEPACKING_GLSL

// NOTE (source): viewZ, the z-coordinate in camera space, is negative for points in front of the camera
// Source: three.js r171 src/renderers/shaders/ShaderChunk/packing.glsl.js:79-82 viewZToOrthographicDepth
float viewZToOrthographicDepth( const in float viewZ, const in float near, const in float far ) {
	// -near maps to 0; -far maps to 1
	return ( viewZ + near ) / ( near - far );
}

// Source: three.js r171 src/renderers/shaders/ShaderChunk/packing.glsl.js:84-87 orthographicDepthToViewZ
float orthographicDepthToViewZ( const in float depth, const in float near, const in float far ) {
	// maps orthographic depth in [ 0, 1 ] to viewZ
	return depth * ( near - far ) - near;
}

// Source: three.js r171 src/renderers/shaders/ShaderChunk/packing.glsl.js:91-94 viewZToPerspectiveDepth
float viewZToPerspectiveDepth( const in float viewZ, const in float near, const in float far ) {
	// -near maps to 0; -far maps to 1
	return ( ( near + viewZ ) * far ) / ( ( far - near ) * viewZ );
}

// Source: three.js r171 src/renderers/shaders/ShaderChunk/packing.glsl.js:96-99 perspectiveDepthToViewZ
float perspectiveDepthToViewZ( const in float depth, const in float near, const in float far ) {
	// maps perspective depth in [ 0, 1 ] to viewZ
	return ( near * far ) / ( ( far - near ) * depth - far );
}
#endif // WOLF_TK_THREEPACKING_GLSL
