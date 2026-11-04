// Zero disables the test; one/two excludes native row parity zero/one.
layout(constant_id = 6) const int HOST_FIELD = 0;
layout(constant_id = 7) const int HOST_SCALE = 1;
void clip_field() {
    if (HOST_FIELD != 0 && ((int(gl_FragCoord.y) / HOST_SCALE) & 1) == HOST_FIELD - 1)
        discard;
}
