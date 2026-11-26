"""讓測試可以 import tools/ 底下的模組（cup_updater 等）"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
