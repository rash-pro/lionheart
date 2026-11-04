"""Comparison tools preserve the user's input card even with a conflicting output."""
import pytest
from tools.compare import run


@pytest.mark.parametrize('symlink', [False, True])
def test_output_collision_preserves_input_card(tmp_path, symlink):
    card = tmp_path / 'card.mcr'
    original = b'owned synthetic card'
    if symlink:
        target = tmp_path / 'source.mcr'
        target.write_bytes(original)
        card.symlink_to(target)
    else:
        card.write_bytes(original)
    with pytest.raises(ValueError, match='input card'):
        run(tmp_path / 'unused-player', tmp_path / 'unused-exe', [],
            tmp_path / 'unused-script', 1, tmp_path, card)
    assert card.read_bytes() == original
    assert card.is_symlink() == symlink
