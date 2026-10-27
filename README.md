# Manifold Builder

Builder tool for Manifold images.

A Manifold image is a [Discoverable Disk Image](https://uapi-group.org/specifications/specs/discoverable_disk_image/)
for use with the [Manifold Deployment System](https://github.com/ximion/manifold). An image is a GPT wrapping a
compressed erofs filesystem, a dm-verity hash partition over it, and room for a signature partition.
`manifold-builder` turns a YAML recipe into such images and bundles them, with their Manifold manifests and
AppStream metadata, in a `.manifold` file that can be signed for upload to a repository.

