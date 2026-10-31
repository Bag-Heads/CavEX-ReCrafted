#version 110

uniform sampler2D tex;
uniform bool enable_texture;
uniform bool enable_alpha;
// render-slice (RENDER_DISPATCH.md): Beta's glAlphaFunc reference value is
// NOT a single constant -- EntityRenderer.renderRainSnow temporarily lowers
// it to 0.01F (softer precipitation-sprite edges) and restores 0.1F when
// done, both different from this engine's own long-standing default (0.0625,
// the literal this uniform replaces). gfx_alpha_test(bool) re-asserts 0.0625
// on EVERY call (not just once at setup), so a prior gfx_alpha_test_ref()
// call can never leak its reference into an unrelated later draw.
uniform float alpha_ref;

uniform bool enable_fog;
uniform vec2 fog_delta;
uniform float fog_distance;
uniform vec3 fog_color;

// audit #17f emissive-tint overlay (creeper white swell flash / mob red hurt flash).
// tint_amount defaults to 0 (set once at gfx_setup) so ALL geometry sharing this
// program -- blocks, items, GUI -- is untinted unless an entity draw opts in via
// gfx_entity_tint(); render_entity.c resets it to 0 right after each tinted mob.
uniform vec3 tint_color;
uniform float tint_amount;

// Per-channel emissive MULTIPLY overlay (Beta RenderSheep glColor3f fleece tint = the wool
// texture multiplied per channel by the fleece colour). Defaults to (1,1,1) = identity (set
// once at gfx_setup); only an entity draw opts in via gfx_entity_modulate() and resets it to
// (1,1,1) right after. Distinct from the tint_amount LERP above so a mob can be both
// fleece-coloured (multiply) and hurt-flashed (lerp) independently.
uniform vec3 tint_modulate;

varying vec3 v_pos;
varying vec4 v_color;
varying vec2 v_texcoord;

void main() {
	vec4 tex_color = vec4(1.0);

	if(enable_texture)
		tex_color = texture2D(tex, v_texcoord);

	float v_fog = 0.0;

	// Beta 1.7.3 terrain fog is GL_LINEAR with start = 0.25*farPlane, end = farPlane
	// (EntityRenderer.setupFog, decompile 740c583) -- a WIDE ramp over the outer 75% of
	// the view, which gives the smooth terrain->sky dissolve. The old (R-9)/8 form was an
	// 8-block hard window at the edge => abrupt horizon. fog_distance == the far edge (R).
	// LOCKSTEP: the Wii fog texture (platform/texture.c gen_texture_fog) mirrors this exact
	// 0.25R..R remap -- edit both together or PC/Wii fog silently drifts.
	if(enable_fog)
		v_fog = clamp((length(fog_delta + v_pos.xz) - fog_distance * 0.25) / (fog_distance * 0.75), 0.0, 1.0);

	vec4 frag = v_color * tex_color;
	// Per-channel MULTIPLY (fleece colour): lit*tex*modulate. Identity (1,1,1) is a no-op.
	// Applied BEFORE the lerp so a draw can be both modulated and flashed.
	frag.rgb *= tint_modulate;
	// Emissive overlay: lerp the lit/textured colour toward the tint. amount==0 is a
	// no-op (frag unchanged), matching the GX backend's amount==0 early-out. Alpha is
	// left untouched so the alpha-test below still culls transparent texels.
	frag.rgb = mix(frag.rgb, tint_color, tint_amount);
	gl_FragColor = vec4(mix(frag.rgb, fog_color, v_fog), frag.a);

	if(enable_alpha && gl_FragColor.a < alpha_ref)
		discard;
}
