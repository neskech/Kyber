// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'frosty_mod_reader.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

FrostyMod _$FrostyModFromJson(Map<String, dynamic> json) => FrostyMod(
  version: (json['version'] as num).toInt(),
  gameVersion: (json['gameVersion'] as num).toInt(),
  filename: json['filename'] as String,
  details: FrostyModDetails.fromJson(json['details'] as Map<String, dynamic>),
  isCollection: json['isCollection'] as bool? ?? false,
  size: (json['size'] as num?)?.toInt() ?? 0,
  offset: (json['offset'] as num?)?.toInt() ?? 0,
  dataCount: (json['dataCount'] as num?)?.toInt() ?? 0,
  dataOffset: (json['dataOffset'] as num?)?.toInt() ?? 0,
  screenshotOffset: (json['screenshotOffset'] as num?)?.toInt() ?? 0,
  mods: (json['mods'] as List<dynamic>?)?.map((e) => e as String).toList(),
  modVersions: (json['modVersions'] as List<dynamic>?)
      ?.map((e) => e as String)
      .toList(),
  icon: const Uint8ListConverter().fromJson(json['icon'] as String?),
  customFrostyData: json['customFrostyData'] == null
      ? null
      : CustomFrostyData.fromJson(
          json['customFrostyData'] as Map<String, dynamic>,
        ),
);

Map<String, dynamic> _$FrostyModToJson(FrostyMod instance) => <String, dynamic>{
  'version': instance.version,
  'gameVersion': instance.gameVersion,
  'filename': instance.filename,
  'size': instance.size,
  'dataOffset': instance.dataOffset,
  'dataCount': instance.dataCount,
  'offset': instance.offset,
  'screenshotOffset': instance.screenshotOffset,
  'isCollection': instance.isCollection,
  'customFrostyData': instance.customFrostyData?.toJson(),
  'icon': const Uint8ListConverter().toJson(instance.icon),
  'mods': instance.mods,
  'modVersions': instance.modVersions,
  'details': instance.details.toJson(),
};

FrostyModDetails _$FrostyModDetailsFromJson(Map<String, dynamic> json) =>
    FrostyModDetails(
      json['name'] as String,
      json['author'] as String,
      json['category'] as String,
      json['version'] as String,
      json['description'] as String,
      json['link'] as String?,
    );

Map<String, dynamic> _$FrostyModDetailsToJson(FrostyModDetails instance) =>
    <String, dynamic>{
      'name': instance.name,
      'author': instance.author,
      'category': instance.category,
      'version': instance.version,
      'description': instance.description,
      'link': instance.link,
    };
