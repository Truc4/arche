// Example Arche program demonstrating all declaration types

arche Particle {
  meta drag: Float
  col pos: Float
  col vel: Float
}

proc init {
  particles = alloc Particle(100)
}

sys move(pos, vel) {
  pos += vel
}

sys damp(vel, drag) {
  vel *= drag
}

func drag_factor(x: Float) -> Float {
  x * 0.98
}
