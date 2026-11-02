// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'frosty_custom_data.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

CustomFrostyData _$CustomFrostyDataFromJson(Map<String, dynamic> json) =>
    CustomFrostyData(
      version: (json['version'] as num).toInt(),
      maps: (json['maps'] as List<dynamic>)
          .map((e) => MapElement.fromJson(e as Map<String, dynamic>))
          .toList(),
      modes: (json['modes'] as List<dynamic>)
          .map((e) => MapElement.fromJson(e as Map<String, dynamic>))
          .toList(),
      modeMappings: (json['modeMappings'] as Map<String, dynamic>?)?.map(
        (k, e) => MapEntry(k, e as String),
      ),
      modeNameOverrides: (json['modeNameOverrides'] as Map<String, dynamic>?)
          ?.map((k, e) => MapEntry(k, e as String)),
    );

Map<String, dynamic> _$CustomFrostyDataToJson(CustomFrostyData instance) =>
    <String, dynamic>{
      'version': instance.version,
      'maps': instance.maps,
      'modes': instance.modes,
      'modeMappings': instance.modeMappings,
      'modeNameOverrides': instance.modeNameOverrides,
    };

MapElement _$MapElementFromJson(Map<String, dynamic> json) => MapElement(
  name: json['name'] as String,
  id: json['id'] as String,
  image: json['image'] as String,
  supportedModes: (json['supportedModes'] as List<dynamic>?)
      ?.map((e) => e as String)
      .toList(),
  maxPlayers: (json['maxPlayers'] as num?)?.toInt(),
);

Map<String, dynamic> _$MapElementToJson(MapElement instance) =>
    <String, dynamic>{
      'name': instance.name,
      'id': instance.id,
      'image': instance.image,
      'supportedModes': instance.supportedModes,
      'maxPlayers': instance.maxPlayers,
    };
