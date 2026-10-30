extends Node3D

var xr_interface: XRInterface
var t := 0.0
var log_cam := false
var card_hidden := false
var cam_frames := 0
var load_rotate := false
var marker: Node3D = null
var marker_text := ""
var marker_timer := 0.0
var marker_follow := false   # ride 'marker_offset' (ahead, right, up) from the head every frame until "drop"
var marker_offset := Vector3.ZERO
var marker_button_t := 0.0   # last controller press that toggled follow/drop (debounce)
var marker_trigger := [false, false]
var load_boxes: Array[Node3D] = []
var panels_layers: Array[Node3D] = []
var panels_toggle := false
var panels_follow := false
var panels_t := 0.0

func _make_load(boxes: int, lights: int) -> void:
	var root := Node3D.new()
	root.name = "Load"
	add_child(root)
	var proto := $Boxes/B1 as MeshInstance3D
	var side := int(ceil(sqrt(float(max(boxes, 1)))))
	var spacing := 0.6
	for i in boxes:
		var m := MeshInstance3D.new()
		m.mesh = proto.mesh
		m.material_override = proto.get_surface_override_material(0)
		var gx: int = i % side
		var gz: int = i / side
		m.position = Vector3((gx - side / 2.0) * spacing, 0.2 + 0.25 * ((gx + gz) % 3), -2.0 - gz * spacing)
		m.scale = Vector3(0.25, 0.25, 0.25)
		root.add_child(m)
		load_boxes.append(m)
	for j in lights:
		var l := OmniLight3D.new()
		l.omni_range = 10.0
		l.light_energy = 1.5
		l.shadow_enabled = true
		var a: float = TAU * float(j) / float(max(lights, 1))
		l.position = Vector3(cos(a) * 4.0, 2.5, -2.0 - side * spacing * 0.5 + sin(a) * 4.0)
		root.add_child(l)

func _ready() -> void:
	xr_interface = XRServer.find_interface("OpenXR")
	# Render supersampling for the sharpness runs (2026-10-19): /tmp/xr-smoke-ss holds a multiplier (e.g. 1.5) read
	# once at start; the runtime still streams its own eye size, so the foveation pass downsamples the bigger render.
	if FileAccess.file_exists("/tmp/xr-smoke-ss"):
		var ss := FileAccess.get_file_as_string("/tmp/xr-smoke-ss").strip_edges().to_float()
		if ss > 0.0 and xr_interface:
			xr_interface.render_target_size_multiplier = ss
			print("xr-smoke: render_target_size_multiplier ", ss)
	# Frame-rate cap for the fresh-frames ladder (2026-10-27): /tmp/xr-smoke-fps holds frames/s (e.g. 45),
	# read once at start. The panel and the runtime stay at their rate; the app submits fewer new frames, so the client
	# decodes fewer. Test-only, like the ss file.
	if FileAccess.file_exists("/tmp/xr-smoke-fps"):
		var cap := FileAccess.get_file_as_string("/tmp/xr-smoke-fps").strip_edges().to_int()
		if cap > 0:
			Engine.max_fps = cap
			print("xr-smoke: max_fps ", cap)
	if xr_interface and xr_interface.initialize():
		print("xr-smoke: OpenXR initialised, runtime=", xr_interface.get_system_info().get("XRRuntimeName", "?"), " ", xr_interface.get_system_info().get("XRRuntimeVersion", ""))
		get_viewport().use_xr = true
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	else:
		print("xr-smoke: OpenXR init FAILED")
	log_cam = OS.get_environment("XR_SMOKE_LOG_CAM") == "1"
	_make_card()
	# World-locked test card (2026-10-19, the first dense wear: the chart vibrated in space - a head-locked
	# object inside a world-locked layer is counter-moved by the compositor's reprojection, so it can never read as
	# still). With /tmp/xr-smoke-world present the card and text rows move from the camera to the origin, 1 m ahead
	# of the start pose at 1.6 m; without it they stay head-locked, which the off-head sharpness dumps need. Runs after _make_card(),
	# which finds the card by its camera path (the first world-locked wear showed a blank brown card).
	if FileAccess.file_exists("/tmp/xr-smoke-world"):
		var origin := get_node_or_null("XROrigin3D")
		if origin:
			for n in ["Card", "Text32", "Text24", "Text16", "Text12"]:
				var node: Node3D = get_node_or_null("XROrigin3D/XRCamera3D/" + n)
				if node:
					var t := node.transform
					node.get_parent().remove_child(node)
					origin.add_child(node)
					node.transform = Transform3D(t.basis, t.origin + Vector3(0, 1.5, 0))
			print("xr-smoke: test card world-locked")
	_poll_card()   # hidden from the first frame when /tmp/xr-smoke-card-off is there
	# Marker cube (2026-10-19, the parallax question): /tmp/xr-smoke-marker holds "x y z [size]" in the stage
	# space (SteamVR standing = OpenXR STAGE, y up, -z forward). A small world-locked cube goes there, so a cube
	# placed just above the WayVR keyboard's measured pose must keep a constant gap to it while the wearer leans; a gap
	# that changes with where they stand, and stays changed, is geometry (scale, IPD, origin), a gap that catches
	# up is latency.
	_poll_marker()
	_make_controllers()
	# Any controller button (or a trigger pull) toggles the can between follow and dropped (2026-10-19:
	# a button press locks it to the world). The Frame client ships A/X/B/Y/menu and the trigger.
	for hand in ["Left", "Right"]:
		var ctrl := get_node_or_null("XROrigin3D/" + hand) as XRController3D
		if ctrl:
			ctrl.button_pressed.connect(_on_marker_button.bind(hand))
			ctrl.input_float_changed.connect(_on_marker_float.bind(hand))
	# Load ladder (2026-10-19: progressively more complicated scenes, to find what the Mac can push stably):
	# /tmp/xr-smoke-load holds "boxes=N lights=M [rotate=1]" read once at start. N extra shadow-casting boxes
	# (one MeshInstance3D each, so N is also N draw calls per eye) in a grid around the origin, M shadow-casting
	# OmniLight3Ds around them. Measured from the Mac log's WaitFrame "app late" count and Godot's own fps line.
	if FileAccess.file_exists("/tmp/xr-smoke-load"):
		var spec := FileAccess.get_file_as_string("/tmp/xr-smoke-load").strip_edges()
		var boxes := 0
		var lights := 0
		for tok in spec.split(" ", false):
			var kv := tok.split("=")
			if kv.size() == 2 and kv[0] == "boxes":
				boxes = int(kv[1])
			if kv.size() == 2 and kv[0] == "lights":
				lights = int(kv[1])
			if kv.size() == 2 and kv[0] == "rotate":
				load_rotate = kv[1] == "1"
		_make_load(boxes, lights)
		print("xr-smoke: load boxes=", boxes, " lights=", lights, " rotate=", load_rotate)
	# Passthrough / alpha (2026-10-19, the AR first rung): with /tmp/xr-smoke-alpha present the viewport clears to
	# transparent, the sky and the floor go, and the OpenXR environment blend mode is ALPHA_BLEND (the OXRSys runtime
	# advertises it with app_alpha_blend_passthrough = true in its toml). The server then flags every frame
	# alpha-blend and composes the scene over black; the Frame client keys black to transparent over the headset's
	# own passthrough, the way oxrsys's Quest client does. Read once at start; remove the file and restart for opaque.
	if FileAccess.file_exists("/tmp/xr-smoke-alpha") and xr_interface:
		var modes := xr_interface.get_supported_environment_blend_modes()
		print("xr-smoke: blend modes supported ", modes)
		var ok := xr_interface.set_environment_blend_mode(XRInterface.XR_ENV_BLEND_MODE_ALPHA_BLEND)
		get_viewport().transparent_bg = true
		var we := $WorldEnvironment as WorldEnvironment
		var env := we.environment.duplicate() as Environment
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0, 0, 0, 0)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.6, 0.6, 0.6)
		we.environment = env
		$Floor.visible = false
		# The test card and text rows go too (2026-10-19 worn: the test card sat behind the cube); the room is the backdrop.
		for n in ["Card", "Text32", "Text24", "Text16", "Text12"]:
			for path in ["XROrigin3D/XRCamera3D/" + n, "XROrigin3D/" + n]:
				var node: Node3D = get_node_or_null(path)
				if node:
					node.visible = false
		print("xr-smoke: alpha blend ", "set" if ok else "REFUSED", ", transparent viewport, floor and card hidden")

	# Composition layers (2026-10-25, line menus-and-panels-in-every-engine): with /tmp/xr-smoke-panels present a
	# world-locked menu QUAD (1.0 x 0.6 m, 1.5 m ahead at 1.5 m) and a CYLINDER (radius 1.5 m, 60 degrees, to the
	# left) are submitted as OpenXR layers, not drawn into the scene. OXRSys used to drop quads and refuse cylinders;
	# the runtime composites both into each eye before encode. FRAMELAB_PANELS=1 does the same as the file (one switch
	# for all three engines, 2026-10-28). "toggle" in either hides and shows both layers every 10 s, so the runtime's
	# "VideoEncoder: overlays:" line splits the GPU cost with and without in one run. "behind" (2026-10-28, v1
	# follow-up 4) submits both BEFORE the projection layer (negative sort_order) with Godot's hole punch, so the
	# scene is drawn in front of them; the runtime shows them through the punched hole (an underlay). "follow" keeps
	# both in front of the head every frame, so an off-head stand dump sees them whatever way the headset faces.
	# "equirect" (2026-10-30, XR_KHR_composition_layer_equirect2) submits one EQUIRECT layer instead: a 120 x 60
	# degree window of a 2 m sphere centred on the head, its image a labelled test card (_equirect_viewport); with
	# "follow" it stays centred on the head. Godot has no cube layer node; the cube test is the native xr-layers app.
	var panels_mode := OS.get_environment("FRAMELAB_PANELS")
	if panels_mode == "" and FileAccess.file_exists("/tmp/xr-smoke-panels"):
		panels_mode = "1 " + FileAccess.get_file_as_string("/tmp/xr-smoke-panels")
	if panels_mode != "" and panels_mode != "0":
		panels_toggle = panels_mode.find("toggle") >= 0
		panels_follow = panels_mode.find("follow") >= 0
		if panels_mode.find("equirect") >= 0:
			_make_equirect(panels_mode.find("behind") >= 0)
		else:
			_make_panels(panels_mode.find("behind") >= 0)

# The sharpness test card (2026-10-19): a 512x512 texture on a 0.6 m quad head-locked 1 m ahead — black 8-texel
# border on mid grey, 8 bands of vertical black/white stripes with texel periods 2,3,4,6,8,12,16,24 (top to bottom,
# 48 texels tall, 12 apart, starting at y 24), plus Label3D rows at font sizes 32/24/16/12 below it. Read by
# tools/sharpness.py from the Frame client's eye dump; keep the layout and that script in sync.
func _make_card() -> void:
	var img := Image.create(512, 512, true, Image.FORMAT_RGB8)
	img.fill(Color(0.5, 0.5, 0.5))
	for y in 512:
		for x in 512:
			if x < 8 or x >= 504 or y < 8 or y >= 504:
				img.set_pixel(x, y, Color.BLACK)
	var periods := [2, 3, 4, 6, 8, 12, 16, 24]
	for i in periods.size():
		var p: int = periods[i]
		for y in range(24 + i * 60, 24 + i * 60 + 48):
			for x in range(24, 488):
				var on := int(floor(float((x - 24) * 2) / float(p))) % 2 == 0
				img.set_pixel(x, y, Color.WHITE if on else Color.BLACK)
	img.generate_mipmaps()
	var tex := ImageTexture.create_from_image(img)
	var card := $XROrigin3D/XRCamera3D/Card as MeshInstance3D
	var mat := (card.get_surface_override_material(0) as StandardMaterial3D).duplicate() as StandardMaterial3D
	mat.albedo_texture = tex
	mat.albedo_color = Color.WHITE
	card.set_surface_override_material(0, mat)

# A SubViewport drawn like a game menu: dark panel, light border, a title and three rows, each a different colour so
# a flipped or mirrored layer reads at a glance (red row on top, the title left-aligned).
func _panel_viewport(title: String, size: Vector2i) -> SubViewport:
	var vp := SubViewport.new()
	vp.size = size
	vp.transparent_bg = true
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var bg := ColorRect.new()
	bg.color = Color(0.08, 0.09, 0.12, 0.85)
	bg.size = Vector2(size)
	vp.add_child(bg)
	var border := ReferenceRect.new()
	border.border_color = Color(0.9, 0.9, 0.9)
	border.border_width = 6.0
	border.editor_only = false
	border.size = Vector2(size)
	vp.add_child(border)
	var label := Label.new()
	label.text = title
	label.position = Vector2(24, 16)
	label.add_theme_font_size_override("font_size", 48)
	vp.add_child(label)
	var colours := [Color(0.9, 0.2, 0.2), Color(0.2, 0.8, 0.3), Color(0.25, 0.45, 0.95)]
	for i in colours.size():
		var row := ColorRect.new()
		row.color = colours[i]
		row.position = Vector2(24, 96 + i * 90)
		row.size = Vector2(size.x - 48, 64)
		vp.add_child(row)
	add_child(vp)
	return vp

func _make_panels(behind: bool) -> void:
	var origin := get_node_or_null("XROrigin3D")
	if origin == null:
		print("xr-smoke: panels need XROrigin3D")
		return
	var quad := OpenXRCompositionLayerQuad.new()
	quad.layer_viewport = _panel_viewport("QUAD LAYER", Vector2i(1000, 600))
	quad.quad_size = Vector2(1.0, 0.6)
	quad.alpha_blend = true
	quad.sort_order = -2 if behind else 1
	quad.enable_hole_punch = behind
	quad.position = Vector3(0, 1.5, -1.5)
	origin.add_child(quad)
	var cyl := OpenXRCompositionLayerCylinder.new()
	cyl.layer_viewport = _panel_viewport("CYLINDER LAYER", Vector2i(1200, 600))
	cyl.radius = 1.5
	cyl.central_angle = deg_to_rad(60.0)
	cyl.aspect_ratio = 2.0
	cyl.alpha_blend = true
	cyl.sort_order = -1 if behind else 2
	cyl.enable_hole_punch = behind
	# The cylinder's axis runs through its position; its image centre sits radius ahead along -Z, so turning the
	# node 60 degrees left puts the panel to the left of the quad, both seen from the start pose.
	cyl.position = Vector3(0, 1.5, 0)
	cyl.rotation = Vector3(0, deg_to_rad(60.0), 0)
	origin.add_child(cyl)
	panels_layers = [quad, cyl]
	print("xr-smoke: panels quad + cylinder layers, toggle=", panels_toggle, " follow=", panels_follow, " behind=", behind,
		" quad supported=", quad.is_natively_supported(), " cylinder supported=", cyl.is_natively_supported())

# The equirect test card, 2048 x 1024 over 120 x 60 degrees (+35 up to -25 down): left of ahead red, right
# green, above the horizon lighter, a white line every 15 degrees of longitude, the horizon in yellow, and
# labels at their angles, so a mirrored, flipped or shifted mapping reads at a glance in an eye dump.
const EQ_CENTRAL := 120.0
const EQ_UPPER := 35.0
const EQ_LOWER := -25.0

func _equirect_viewport() -> SubViewport:
	var w := 2048
	var h := 1024
	var vp := SubViewport.new()
	vp.size = Vector2i(w, h)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var horizon := int(round(EQ_UPPER / (EQ_UPPER - EQ_LOWER) * h))
	for quarter in 4:
		var r := ColorRect.new()
		var left := quarter % 2 == 0
		var top := quarter < 2
		r.color = Color(0.55, 0.12, 0.12) if left else Color(0.12, 0.45, 0.15)
		if top:
			r.color = r.color.lightened(0.35)
		r.position = Vector2(0 if left else w / 2, 0 if top else horizon)
		r.size = Vector2(w / 2, horizon if top else h - horizon)
		vp.add_child(r)
	for deg in range(-60, 61, 15):
		var x := (float(deg) / EQ_CENTRAL + 0.5) * w
		var line := ColorRect.new()
		line.color = Color.WHITE
		line.position = Vector2(clampf(x - 3, 0, w - 6), 0)
		line.size = Vector2(6, h)
		vp.add_child(line)
	var hl := ColorRect.new()
	hl.color = Color(1.0, 0.9, 0.1)
	hl.position = Vector2(0, horizon - 4)
	hl.size = Vector2(w, 8)
	vp.add_child(hl)
	var labels := [["AHEAD", 0.0, 10.0], ["LEFT 30", -30.0, 10.0], ["RIGHT 30", 30.0, 10.0],
		["UP 25", 0.0, 25.0], ["DOWN 15", 0.0, -15.0], ["EQUIRECT", -52.0, 28.0]]
	for l in labels:
		var label := Label.new()
		label.text = l[0]
		label.add_theme_font_size_override("font_size", 64)
		label.add_theme_color_override("font_color", Color.WHITE)
		label.add_theme_color_override("font_outline_color", Color.BLACK)
		label.add_theme_constant_override("outline_size", 12)
		var x := (float(l[1]) / EQ_CENTRAL + 0.5) * w
		var y := (EQ_UPPER - float(l[2])) / (EQ_UPPER - EQ_LOWER) * h
		label.position = Vector2(x + 12, y - 40)
		vp.add_child(label)
	add_child(vp)  # a SubViewport outside the tree never renders: the layer would show black
	return vp

func _make_equirect(behind: bool) -> void:
	var origin := get_node_or_null("XROrigin3D")
	if origin == null:
		print("xr-smoke: equirect needs XROrigin3D")
		return
	var eq := OpenXRCompositionLayerEquirect.new()
	eq.layer_viewport = _equirect_viewport()
	eq.radius = 2.0
	eq.central_horizontal_angle = deg_to_rad(EQ_CENTRAL)
	eq.upper_vertical_angle = deg_to_rad(EQ_UPPER)
	eq.lower_vertical_angle = deg_to_rad(-EQ_LOWER)  # Godot takes it positive and submits it negated
	eq.sort_order = -1 if behind else 1
	eq.enable_hole_punch = behind
	eq.position = Vector3(0, 1.6, 0)
	origin.add_child(eq)
	panels_layers = [eq]
	print("xr-smoke: panels equirect layer (2 m, ", EQ_CENTRAL, " x ", EQ_UPPER, "/", EQ_LOWER, " deg), toggle=",
		panels_toggle, " follow=", panels_follow, " behind=", behind, " supported=", eq.is_natively_supported())
	# supported=false means Godot draws its own sphere mesh into the scene instead (the fallback), so an eye
	# dump would show the card without the runtime doing anything: proof needs supported=true.

# One source-side snapshot of the rendered viewport (the left eye, layer 0) when /tmp/xr-smoke-snap exists:
# touch that file over ssh, the next frame writes /tmp/xr-smoke-eye.png and removes the flag. Off by default
# so a wear never pays the stall.
func _maybe_snap() -> void:
	if not FileAccess.file_exists("/tmp/xr-smoke-snap"):
		return
	DirAccess.remove_absolute("/tmp/xr-smoke-snap")
	# In XR the viewport texture is a 2-layer array: get_image() comes back black, so read layer 0 (the left eye).
	var im := RenderingServer.texture_2d_layer_get(get_viewport().get_texture().get_rid(), 0)
	if im and not im.is_empty():
		im.save_png("/tmp/xr-smoke-eye.png")
		print("xr-smoke: snapshot ", im.get_width(), "x", im.get_height(), " -> /tmp/xr-smoke-eye.png")
	else:
		print("xr-smoke: snapshot failed (no image)")

func _process(delta: float) -> void:
	marker_timer += delta
	if marker_timer > 1.0:
		marker_timer = 0.0
		_poll_marker()
		_poll_card()
	if marker_follow and marker:
		marker.global_position = _head_relative(marker_offset)
	if panels_follow and panels_layers.size() == 2:
		# Origin space: the quad 1.5 m ahead of the head, the cylinder's axis through the head with its image 40
		# degrees to the left and 0.35 m lower, so the two do not cover each other.
		var head := ($XROrigin3D/XRCamera3D as XRCamera3D).transform
		panels_layers[0].transform = head * Transform3D(Basis(), Vector3(0, 0, -1.5))
		panels_layers[1].transform = head * Transform3D(Basis(Vector3.UP, deg_to_rad(40.0)), Vector3(0, -0.35, 0))
	elif panels_follow and panels_layers.size() == 1:
		panels_layers[0].transform = ($XROrigin3D/XRCamera3D as XRCamera3D).transform
	t += delta
	for c in $Boxes.get_children():
		c.rotation.y = t * 0.7 + c.position.x
		c.rotation.x = t * 0.3
	if panels_toggle and not panels_layers.is_empty():
		panels_t += delta
		if panels_t > 10.0:
			panels_t = 0.0
			for layer in panels_layers:
				layer.visible = not layer.visible
			print("xr-smoke: panels ", "shown" if panels_layers[0].visible else "hidden")
	if load_rotate:
		for m in load_boxes:
			m.rotation.y = t * 0.7 + m.position.x
	if Engine.get_process_frames() % 90 == 0:
		_maybe_snap()
	if Engine.get_process_frames() % 300 == 0:
		print("xr-smoke: frame ", Engine.get_process_frames(), " fps ", Engine.get_frames_per_second())
	if log_cam:
		# One line per rendered frame: what the runtime handed us as the head pose.
		var cam := $XROrigin3D/XRCamera3D as XRCamera3D
		var o := cam.global_transform.origin
		var e := cam.global_transform.basis.get_euler()
		print("xr-smoke: cam t=%.4f pos=%.4f,%.4f,%.4f eul=%.4f,%.4f,%.4f" % [Time.get_ticks_usec() / 1000000.0, o.x, o.y, o.z, e.x, e.y, e.z])

# The controllers, as the Unity and Unreal smokes show them (2026-10-26): a block at the grip pose
# and a 1 m ray from the aim pose, which on the Frame sits about 40 degrees from the grip. Godot names the poses after
# the action map's grip_pose / aim_pose ("grip", "aim"). Every press and release is logged ("xr-smoke: input ..."), and
# each pose's tracking changes, so a worn check walks the buttons and reads the log. The block also shows trigger
# and grip, and the can (above) is the thing to grab.
var input_down := {}
# As in the Unity and Unreal smokes: the trigger ramps the block from grey to orange, the grip squeezes it to
# half its length. Presses arrive whether or not a scene shows them; this makes them visible worn.
const BLOCK_REST := Color(0.55, 0.55, 0.6)
const BLOCK_PULLED := Color(1.0, 0.45, 0.1)

func _on_float(hand: String, block: MeshInstance3D, name: String, value: float) -> void:
	_log_level(hand, name, value > 0.5)
	if block == null:
		return
	if name == "trigger":
		(block.material_override as StandardMaterial3D).albedo_color = BLOCK_REST.lerp(BLOCK_PULLED, clampf(value, 0.0, 1.0))
	elif name == "grip":
		block.scale = Vector3(1.0, 1.0, 1.0 - 0.5 * clampf(value, 0.0, 1.0))

func _make_controllers() -> void:
	var origin := get_node_or_null("XROrigin3D")
	if origin == null:
		return
	for hand in ["Left", "Right"]:
		var tracker := &"left_hand" if hand == "Left" else &"right_hand"
		var grip := get_node_or_null("XROrigin3D/" + hand) as XRController3D
		if grip == null:
			continue
		grip.pose = &"grip"
		var block := grip.get_node_or_null(hand + "Mesh") as MeshInstance3D
		if block:
			var bm := BoxMesh.new()
			bm.size = Vector3(0.04, 0.06, 0.12)
			block.mesh = bm
			block.transform = Transform3D.IDENTITY
			var bmat := StandardMaterial3D.new()
			bmat.albedo_color = BLOCK_REST
			block.material_override = bmat
		var aim := XRController3D.new()
		aim.name = hand + "Aim"
		aim.tracker = tracker
		aim.pose = &"aim"
		origin.add_child(aim)
		var ray := MeshInstance3D.new()
		var cm := CylinderMesh.new()
		cm.top_radius = 0.004
		cm.bottom_radius = 0.004
		cm.height = 1.0
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(0.95, 0.6, 0.2)
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		cm.material = mat
		ray.mesh = cm
		# The cylinder's axis is Y; lay it along the aim's forward (-Z), starting at the aim origin.
		ray.transform = Transform3D(Basis(Vector3.RIGHT, -PI / 2), Vector3(0, 0, -0.5))
		aim.add_child(ray)
		grip.button_pressed.connect(func(n: String): _log_input(hand, n, true))
		grip.button_released.connect(func(n: String): _log_input(hand, n, false))
		grip.input_float_changed.connect(func(n: String, v: float): _on_float(hand, block, n, v))
		grip.input_vector2_changed.connect(func(n: String, v: Vector2): _log_level(hand, n, v.length() > 0.5))
		grip.tracking_changed.connect(func(t: bool): print("xr-smoke: input ", hand, " grip tracking ", t))
		aim.tracking_changed.connect(func(t: bool): print("xr-smoke: input ", hand, " aim tracking ", t))
	print("xr-smoke: controllers made (block at the grip, ray from the aim pose)")

func _log_input(hand: String, name: String, down: bool) -> void:
	input_down[hand + " " + name] = down
	print("xr-smoke: input ", hand, " ", name, " down" if down else " up")

func _log_level(hand: String, name: String, down: bool) -> void:
	if input_down.get(hand + " " + name, false) != down:
		_log_input(hand, name, down)

func _on_marker_float(name: String, value: float, hand: String) -> void:
	if not name.begins_with("trigger"):
		return
	var h := 0 if hand == "Left" else 1
	var down := value > 0.6
	if down and not marker_trigger[h]:
		_on_marker_button(name, hand)
	marker_trigger[h] = down

func _on_marker_button(name: String, hand: String) -> void:
	# Capacitive touch actions (trigger_touch, thumbstick_touch, ...) fire from a resting finger: on the first
	# wear (2026-10-20 09:02) every toggle came from "Left trigger_touch". Only clicks and pulls count.
	if name.ends_with("_touch"):
		return
	var now := Time.get_ticks_msec() / 1000.0
	if now - marker_button_t < 0.5 or marker == null:
		return
	marker_button_t = now
	if marker_follow:
		marker_follow = false
		print("xr-smoke: can dropped at ", marker.global_position, " (", hand, " ", name, ")")
	else:
		marker_follow = true
		print("xr-smoke: can picked up, following ", marker_offset, " (", hand, " ", name, ")")

func _head_relative(off: Vector3) -> Vector3:
	var cam := $XROrigin3D/XRCamera3D as XRCamera3D
	var fwd: Vector3 = -cam.global_transform.basis.z
	fwd.y = 0.0
	fwd = fwd.normalized()
	var right: Vector3 = fwd.cross(Vector3.UP)
	return cam.global_position + fwd * off.x + right * off.y + Vector3(0, off.z, 0)

func _make_can(scale: float) -> Node3D:
	# A 12 oz can to scale (2026-10-19), so a real-world object can be matched up to it: 66 mm across, 122.6 mm tall, red body, white band, grey lid. Origin at the base.
	var root := Node3D.new()
	var body := MeshInstance3D.new()
	var cm := CylinderMesh.new(); cm.top_radius = 0.033 * scale; cm.bottom_radius = 0.033 * scale; cm.height = 0.1226 * scale
	body.mesh = cm
	var red := StandardMaterial3D.new(); red.albedo_color = Color(0.85, 0.05, 0.05); body.material_override = red
	body.position.y = 0.0613 * scale
	root.add_child(body)
	var band := MeshInstance3D.new()
	var bm := CylinderMesh.new(); bm.top_radius = 0.0332 * scale; bm.bottom_radius = 0.0332 * scale; bm.height = 0.03 * scale
	band.mesh = bm
	var white := StandardMaterial3D.new(); white.albedo_color = Color(0.95, 0.95, 0.95); band.material_override = white
	band.position.y = 0.0613 * scale
	root.add_child(band)
	var lid := MeshInstance3D.new()
	var lm := CylinderMesh.new(); lm.top_radius = 0.030 * scale; lm.bottom_radius = 0.033 * scale; lm.height = 0.004 * scale
	lid.mesh = lm
	var grey := StandardMaterial3D.new(); grey.albedo_color = Color(0.75, 0.75, 0.78); grey.metallic = 0.6; lid.material_override = grey
	lid.position.y = 0.1246 * scale
	root.add_child(lid)
	return root

# Test card on/off (2026-10-28: a head-locked chart in the middle of the view is uncomfortable worn).
# /tmp/xr-smoke-card-off present = the card and its four text rows are hidden;
# re-read every second, so `frame-stream card off|on` acts on a running scene. Absent (the default) = shown, which
# the off-head sharpness dumps (tools/sharpness.py) need.
func _poll_card() -> void:
	var off := FileAccess.file_exists("/tmp/xr-smoke-card-off")
	if off == card_hidden:
		return
	card_hidden = off
	for n in ["Card", "Text32", "Text24", "Text16", "Text12"]:
		for base in ["XROrigin3D/XRCamera3D/", "XROrigin3D/"]:
			var node: Node3D = get_node_or_null(base + n)
			if node:
				node.visible = not off
	print("xr-smoke: test card ", "hidden" if off else "shown")


func _poll_marker() -> void:
	# /tmp/xr-smoke-marker, re-read every second, acted on when its text changes:
	#   "x y z [scale]"             world-lock the can with its base at stage (x, y, z)
	#   "head ahead right up [scale]"  world-lock it once, metres from the head along its yaw-only forward
	#   "follow ahead right up [scale]"  ride with the head (like a panel being carried) until...
	#   "drop"                      ...world-lock it where it is now
	if not FileAccess.file_exists("/tmp/xr-smoke-marker"):
		return
	var txt := FileAccess.get_file_as_string("/tmp/xr-smoke-marker").strip_edges()
	if txt == marker_text:
		return
	marker_text = txt
	var mtoks := txt.split(" ", false)
	var morigin := get_node_or_null("XROrigin3D")
	if mtoks.size() < 1 or not morigin:
		return
	if mtoks[0] == "drop":
		if marker:
			marker_follow = false
			print("xr-smoke: can dropped at ", marker.global_position)
		return
	var mode := mtoks[0] if mtoks[0] in ["head", "follow"] else "abs"
	if mode != "abs":
		mtoks = mtoks.slice(1)
	if mtoks.size() < 3:
		return
	var mscale: float = float(mtoks[3]) if mtoks.size() > 3 else 1.0
	if marker:
		marker.queue_free()
	marker = _make_can(mscale)
	morigin.add_child(marker)
	var v := Vector3(float(mtoks[0]), float(mtoks[1]), float(mtoks[2]))
	marker_follow = (mode == "follow")
	marker_offset = v
	if mode == "abs":
		marker.position = v
		print("xr-smoke: can at ", marker.position, " scale ", mscale)
	else:
		marker.global_position = _head_relative(v)
		print("xr-smoke: can ", mode, " ", v.x, " m ahead of the head, ", v.y, " right, ", v.z, " up -> ", marker.global_position, " scale ", mscale)
