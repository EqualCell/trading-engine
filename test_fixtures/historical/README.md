# Synthetic parser test fixtures — NOT historical market data

These tiny, manually specified CSVs test the Tardis-shaped parser. They are not
provider downloads and must not be presented as real recorded market activity.
They include a trade before the first book, equal capture timestamps, a quoted
trade ID, exact eight-place amounts/prices, and trailing missing depth levels.
The test harness creates temporary provenance with checksums for these fixtures.
