#include "Grid.h"

#include "UABAssert.h"

using namespace UAlbertaBot;

// Create an empty, unitialized, unusable grid.
// Necessary if a Grid subclass is created before BWAPI is initialized.
Grid::Grid()
{
}

// Create an initialized grid, given the size.
Grid::Grid(int w, int h, int value)
    : width(w)
    , height(h)
    , grid(w, std::vector<short>(h, value))
{
}

int Grid::get(int x, int y) const
{
    UAB_ASSERT(grid.size() == width && width > 0 && x >= 0 && y >= 0 && x < width && y < height,
        "bad at(%d,%d) limit(%d,%d) size %dx%d", x, y, width, height, grid.size(), grid[0].size());

    return grid[x][y];
}

int Grid::at(int x, int y) const
{
    return get(x, y);
}

int Grid::at(const BWAPI::TilePosition & pos) const
{
    return at(pos.x, pos.y);
}

int Grid::at(const BWAPI::WalkPosition & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int Grid::at(const BWAPI::Position & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int Grid::at(BWAPI::Unit unit) const
{
    return at(unit->getTilePosition());
}

// Check the correct shape of the data structure.
void Grid::selfTest(const std::string & message) const
{
    UAB_ASSERT(width > 0 && width < 256 && height > 0 && height < 256 && grid.size() == width,
        "bad size %dx%d (size %d)", width, height, grid.size());
    for (size_t x = 0; x < grid.size(); ++x)
    {
        UAB_ASSERT(grid[x].size() == height, "%s: bad grid[%d] height %d in %dx%d", message.c_str(), x, grid[x].size(), width, height);
    }
}

// Draw a number in each tile.
// This default method is overridden in some subclasses.
void Grid::draw() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            int n = grid[x][y];
            if (n)
            {
                char color = n < 0 ? purple : gray;
                BWAPI::Broodwar->drawTextMap(
                    BWAPI::Position(x * 32 + 8, y * 32 + 8),
                    "%c%d", color, n);
            }
        }
    }
}