from tools.verify_host_pad import field_positions, protocol_matches, walking_changed, write_sequence


def test_facing_changes_do_not_count_as_walking():
    idle = "PAD_FIELD vblank=1850 map=bghoke_2 x=-696320 y=3366912 z=0 facing=64"
    turned = idle.replace("facing=64", "facing=75")
    assert not walking_changed(field_positions(idle, 1849, 1851), field_positions(turned, 1849, 1851))


def test_walking_requires_corresponding_samples():
    assert not walking_changed({}, {})
    assert not walking_changed({1850: ("bghoke_2", 0, 0, 0)}, {})
    assert not walking_changed({1850: ("bghoke_2", 0, 0, 0)}, {1851: ("bghoke_2", 1, 0, 0)})


def test_translation_and_field_transitions_count_as_walking():
    idle = {1850: ("bghoke_2", 1, 2, 3)}
    assert walking_changed(idle, {1850: ("bghoke_2", 2, 2, 3)})
    assert walking_changed(idle, {1850: ("bghoke_1", 1, 2, 3)})
    assert walking_changed({**idle, 1851: idle[1850]}, {1850: ("bghoke_1", 1, 2, 3)})


def test_probe_schedule_preserves_the_route_and_centers_idle_axes(tmp_path):
    path = tmp_path / "input.txt"
    for mode, expected in (("idle", ["0000", "0", "0", "0", "0"]),
                           ("digital", ["0020", "0", "0", "0", "0"]),
                           ("axis", ["0000", "32767", "0", "0", "0"])):
        write_sequence(path, 2400, 2600, "RIGHT", [(1700, 1900, "DOWN")], mode)
        events = {int(frame): values for frame, *values in map(str.split, path.read_text().splitlines())}
        assert events[1700] == ["0040", "0", "0", "0", "0"]
        assert events[1900] == ["0000", "0", "0", "0", "0"]
        assert events[2400] == expected
        assert events[2600] == ["0000", "0", "0", "0", "0"]


def test_every_observed_poll_must_match_even_when_a_vblank_is_skipped():
    reply = bytes.fromhex("ff735adfff80808080")
    polls = [(frame, reply) for frame in range(1701, 1900) if frame != 1727]
    assert protocol_matches(polls, reply, 1700, 1900)
    polls[20] = (polls[20][0], bytes.fromhex("ff735adfff7f808080"))
    assert not protocol_matches(polls, reply, 1700, 1900)
    assert not protocol_matches([], reply, 1700, 1900)
    assert not protocol_matches(polls[:3], reply, 1700, 1900)
