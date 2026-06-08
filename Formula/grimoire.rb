class Grimoire < Formula
  desc "Terminal-based flashcard drill system with persistent mastery tracking"
  homepage "https://github.com/AngeloNicolson/grimoire"
  url "https://github.com/AngeloNicolson/grimoire/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "9a6459229d9c0711ae9510a5ae802b8061e384ac505c98c00d8e33ab3fb67283"
  license "MIT"

  depends_on "cmake" => :build

  uses_from_macos "ncurses"

  def install
    system "cmake", "-B", "build", *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/grimoire"
  end

  test do
    assert_match "grimoire", shell_output("#{bin}/grimoire --version")
  end
end
