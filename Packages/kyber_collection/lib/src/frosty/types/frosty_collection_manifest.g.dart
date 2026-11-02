// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'frosty_collection_manifest.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

_FrostyCollectionManifest _$FrostyCollectionManifestFromJson(
  Map<String, dynamic> json,
) => _FrostyCollectionManifest(
  link: json['link'] as String,
  title: json['title'] as String,
  author: json['author'] as String,
  version: json['version'] as String,
  description: json['description'] as String,
  category: json['category'] as String,
  mods: (json['mods'] as List<dynamic>).map((e) => e as String).toList(),
  modVersions: (json['modVersions'] as List<dynamic>)
      .map((e) => e as String)
      .toList(),
);

Map<String, dynamic> _$FrostyCollectionManifestToJson(
  _FrostyCollectionManifest instance,
) => <String, dynamic>{
  'link': instance.link,
  'title': instance.title,
  'author': instance.author,
  'version': instance.version,
  'description': instance.description,
  'category': instance.category,
  'mods': instance.mods,
  'modVersions': instance.modVersions,
};
