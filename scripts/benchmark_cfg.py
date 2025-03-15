from yacs.config import CfgNode
import pathlib


def read_yaml_cfg() -> CfgNode:
    cfg = CfgNode()
    cfg.set_new_allowed(True)
    cfg.merge_from_file(str(pathlib.Path(__file__).parent.resolve().joinpath("specs.yaml")))
    cfg.freeze()
    return cfg
