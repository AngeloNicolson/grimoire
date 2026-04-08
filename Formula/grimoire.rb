class Grimoire < Formula
  desc "Terminal-based flashcard drill system with persistent mastery tracking"
  homepage "https://github.com/AngeloNicolson/grimoire"
  url "https://github.com/AngeloNicolson/grimoire/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "31d0f545e7baf798df3f5217365e9c3a6c060dfc0c6a656c42f3d914d8935889"
  license "MIT"

  depends_on "cmake" => :build

  uses_from_macos "ncurses"

  def install
    system "cmake", "-B", "build", *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/grimoire"
  end

  test do
    assert_match "grimoire", shell_output("#{bin}/grimoire --help 2>&1", 1)
  end
end
